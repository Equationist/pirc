/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "lower/lower.h"

/* =========================================================================
 * General PIR LIR Small-Function Inliner
 * =========================================================================
 * Inlines small defined direct callees (no weak linkage, no variadics, no
 * sret/multi-reg hidden return ABI, no dynamic stack allocation) into callers
 * using PIR's block-parameter SSA directly (no phi-node repair needed!).
 */

#define INLINE_MAX_INSTS_GENERAL     48u
#define INLINE_MAX_BLOCKS_GENERAL    6u
#define INLINE_MAX_INSTS_SINGLE_CALL 180u
#define INLINE_MAX_BLOCKS_SINGLE_CALL 24u
#define INLINE_MAX_CALLER_INSTS      2000u
#define INLINE_MAX_ADDED_PER_CALLER  600u

static uint32_t find_func_by_sym(const PirModule *mod, const PirOperand *sym_op) {
    if (sym_op->kind != PIR_OPER_SYM || !sym_op->u.sym.name) return PIR_INVALID_ID;
    if (sym_op->u.sym.is_func && sym_op->u.sym.id < mod->funcs.len) {
        const PirFunction *f = &mod->funcs.data[sym_op->u.sym.id];
        if (f->name && strcmp(f->name, sym_op->u.sym.name) == 0) {
            return sym_op->u.sym.id;
        }
    }
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (mod->funcs.data[i].name && strcmp(mod->funcs.data[i].name, sym_op->u.sym.name) == 0) {
            return i;
        }
    }
    return PIR_INVALID_ID;
}

static bool is_callee_inlineable(const PirFunction *callee, const PirCfg *ccfg,
                                 uint32_t total_calls_to_callee, uint32_t *out_inst_count,
                                 uint64_t *out_alloca_bytes) {
    if (!callee->is_defined) return false;
    if (callee->linkage == PIR_LINK_WEAK) return false;
    if (callee->is_variadic) return false;
    if (callee->cabi_ret.kind != PIR_CABI_RET_NONE) return false;
    if (callee->blocks.len == 0u || ccfg->rpo_len == 0u) return false;
    if (callee->blocks.data[0].params.len != 0u) return false;

    bool single_internal = (callee->linkage == PIR_LINK_INTERNAL && total_calls_to_callee == 1u);
    uint32_t max_blocks = single_internal ? INLINE_MAX_BLOCKS_SINGLE_CALL : INLINE_MAX_BLOCKS_GENERAL;
    uint32_t max_insts  = single_internal ? INLINE_MAX_INSTS_SINGLE_CALL  : INLINE_MAX_INSTS_GENERAL;
    if (ccfg->rpo_len > max_blocks) return false;

    for (uint32_t k = 0u; k < callee->param_count; ++k) {
        if (callee->params[k].abi_byval_size > 0u ||
            callee->params[k].val_id == PIR_INVALID_ID ||
            callee->params[k].val_id >= callee->values.len) {
            return false;
        }
    }

    uint32_t n_insts = 0u;
    uint32_t n_rets  = 0u;
    uint64_t alloca_bytes = 0u;
    bool has_call    = false;
    for (uint32_t ri = 0u; ri < ccfg->rpo_len; ++ri) {
        PirBlockId cb = ccfg->rpo[ri];
        const PirBlock *blk = &callee->blocks.data[cb];
        n_insts += blk->insts.len;
        if (n_insts > max_insts) return false;

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &callee->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_ALLOCA_DYN || ins->op == PIR_OP_STACKSAVE ||
                ins->op == PIR_OP_STACKRESTORE || ins->op == PIR_OP_VA_START) {
                return false;
            }
            if (ins->op == PIR_OP_ALLOCA_PTR) {
                if (cb != 0u) return false;
                if (ins->imm_u64 > 4096u || alloca_bytes > 4096u - ins->imm_u64) {
                    return false;
                }
                alloca_bytes += ins->imm_u64;
            }
            /* Reject any HIR-only opcodes defensively */
            if (ins->op == PIR_OP_ALLOCA || ins->op == PIR_OP_FIELD_PTR ||
                ins->op == PIR_OP_ELEM_PTR || ins->op == PIR_OP_MEMCPY ||
                ins->op == PIR_OP_MEMMOVE || ins->op == PIR_OP_MEMSET ||
                ins->op == PIR_OP_VA_ARG || ins->op == PIR_OP_VA_ARG_BUF ||
                ins->op == PIR_OP_VA_COPY || ins->op == PIR_OP_VA_END ||
                ins->op == PIR_OP_INVOKE || ins->op == PIR_OP_INVOKE_INDIRECT ||
                ins->op == PIR_OP_RESUME) {
                return false;
            }
            if (ins->op == PIR_OP_CALL_INDIRECT) {
                has_call = true;
            }
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u &&
                ins->operands[0].kind == PIR_OPER_SYM) {
                has_call = true;
                /* Never inline returns_twice functions (setjmp / shadow-stack invoke frames / vfork). */
                if (pir_is_returns_twice_name(ins->operands[0].u.sym.name)) {
                    return false;
                }
            }
            if (ins->op == PIR_OP_RET) {
                n_rets++;
            }
        }
    }

    if (n_rets == 0u) return false;
    /* Avoid inlining non-leaf multi-call callees larger than 24 insts to bound register pressure across calls. */
    if (!single_internal && has_call && n_insts > 24u) return false;
    *out_inst_count   = n_insts;
    *out_alloca_bytes = alloca_bytes;
    return true;
}

static bool call_site_matches_callee(const PirFunction *caller, const PirInst *call_ins,
                                     const PirFunction *callee) {
    if (call_ins->operand_count != 1u + callee->param_count) return false;
    if (call_ins->results[1] != PIR_INVALID_ID) return false;

    bool callee_void = (callee->ret_type.kind == PIR_TYPE_SCALAR &&
                        callee->ret_type.u.scalar == PIR_SCALAR_VOID);
    if (callee_void) {
        if (call_ins->results[0] != PIR_INVALID_ID) return false;
    } else if (call_ins->results[0] != PIR_INVALID_ID) {
        if (call_ins->results[0] >= caller->values.len) return false;
        if (!pir_type_eq(caller->values.data[call_ins->results[0]].type, callee->ret_type)) {
            return false;
        }
    }

    for (uint32_t k = 0u; k < callee->param_count; ++k) {
        const PirOperand *arg = &call_ins->operands[1u + k];
        if (arg->kind == PIR_OPER_VAL) {
            if (arg->u.val >= caller->values.len) return false;
            if (!pir_type_eq(caller->values.data[arg->u.val].type, callee->params[k].type)) {
                return false;
            }
        } else if (arg->kind == PIR_OPER_IMM_INT || arg->kind == PIR_OPER_IMM_FLOAT ||
                   arg->kind == PIR_OPER_SYM) {
            /* Materialized via PIR_OP_CONST in caller before entry jump */
        } else {
            return false;
        }
    }
    return true;
}

static PirOperand remap_operand(PirOperand op, const PirValueId *val_map, uint32_t callee_nvals) {
    if (op.kind == PIR_OPER_VAL && op.u.val < callee_nvals && val_map[op.u.val] != PIR_INVALID_ID) {
        op.u.val = val_map[op.u.val];
    }
    return op;
}

static PirBranchTarget clone_target(PirModule *mod, const PirBranchTarget *src,
                                    const PirBlockId *blk_map, uint32_t callee_nblks,
                                    const PirValueId *val_map, uint32_t callee_nvals) {
    PirBranchTarget dst;
    memset(&dst, 0, sizeof(dst));
    dst.target = (src->target < callee_nblks) ? blk_map[src->target] : src->target;
    dst.arg_count = src->arg_count;
    if (src->arg_count > 0u) {
        dst.args = pir_alloc_ops(mod, src->arg_count);
        for (uint32_t i = 0u; i < src->arg_count; ++i) {
            dst.args[i] = remap_operand(src->args[i], val_map, callee_nvals);
        }
    }
    return dst;
}

static void inline_call_site(PirModule *mod, PirFunction *caller,
                             PirBlockId caller_bid, uint32_t call_pos,
                             const PirFunction *callee, const PirCfg *ccfg) {
    PirInstId call_iid = caller->blocks.data[caller_bid].insts.data[call_pos];
    PirValueId ret_vid = caller->insts.data[call_iid].results[0];
    uint32_t call_loc  = caller->insts.data[call_iid].loc_idx;
    uint32_t call_line = caller->insts.data[call_iid].src_line;
    const PirOperand *call_args = caller->insts.data[call_iid].operands + 1u;

    uint32_t callee_nvals = callee->values.len;
    uint32_t callee_nblks = callee->blocks.len;
    PirValueId *val_map = (PirValueId *)malloc(((size_t)callee_nvals + 1u) * sizeof(PirValueId));
    PirBlockId *blk_map = (PirBlockId *)malloc(((size_t)callee_nblks + 1u) * sizeof(PirBlockId));
    if (!val_map || !blk_map) abort();
    for (uint32_t v = 0u; v < callee_nvals; ++v) val_map[v] = PIR_INVALID_ID;
    for (uint32_t b = 0u; b < callee_nblks; ++b) blk_map[b] = PIR_INVALID_ID;

    /* 1. Map callee parameters; materialize immediate/symbol args via PIR_OP_CONST. */
    PirInstIdVec arg_consts = {0};
    for (uint32_t k = 0u; k < callee->param_count; ++k) {
        PirValueId pv = callee->params[k].val_id;
        PirOperand arg = call_args[k];
        if (arg.kind == PIR_OPER_VAL) {
            val_map[pv] = arg.u.val;
        } else {
            PirType pty = callee->params[k].type;
            PirInstId ciid = caller->insts.len;
            PirValueId nv = pir_fn_new_val(mod, caller, "iv", pty, caller_bid, ciid, 0u);
            PirOpcode cop = (arg.kind == PIR_OPER_SYM) ? PIR_OP_ADDR : PIR_OP_CONST;
            PirInst ci = pir_make_inst(cop, pty, call_line, call_loc);
            ci.operands      = pir_alloc_ops(mod, 1u);
            ci.operands[0]   = arg;
            ci.operands[0].type = pty;
            ci.operand_count = 1u;
            ci.results[0]    = nv;
            PIR_VEC_PUSH(&caller->insts, ci);
            PIR_VEC_PUSH(&arg_consts, ciid);
            val_map[pv] = nv;
        }
    }

    /* 2. Create continuation block and split caller_bid right after call_pos. */
    PirBlockId cont_bid = pir_fn_new_block(mod, caller, "inl_c", call_line);
    if (ret_vid != PIR_INVALID_ID) {
        PIR_VEC_PUSH(&caller->blocks.data[cont_bid].params, ret_vid);
        caller->values.data[ret_vid].def_block   = cont_bid;
        caller->values.data[ret_vid].def_inst    = PIR_INVALID_ID;
        caller->values.data[ret_vid].param_index = 0u;
    }

    PirInstIdVec kept_allocas = {0};
    uint32_t orig_len = caller->blocks.data[caller_bid].insts.len;
    for (uint32_t j = call_pos + 1u; j < orig_len; ++j) {
        PirInstId miid = caller->blocks.data[caller_bid].insts.data[j];
        if (caller_bid == 0u && caller->insts.data[miid].op == PIR_OP_ALLOCA_PTR) {
            PIR_VEC_PUSH(&kept_allocas, miid);
            continue;
        }
        PIR_VEC_PUSH(&caller->blocks.data[cont_bid].insts, miid);
        PirInst *mi = &caller->insts.data[miid];
        if (mi->results[0] != PIR_INVALID_ID) {
            caller->values.data[mi->results[0]].def_block = cont_bid;
        }
        if (mi->results[1] != PIR_INVALID_ID) {
            caller->values.data[mi->results[1]].def_block = cont_bid;
        }
    }
    caller->blocks.data[caller_bid].insts.len = call_pos;
    for (uint32_t j = 0u; j < kept_allocas.len; ++j) {
        PIR_VEC_PUSH(&caller->blocks.data[caller_bid].insts, kept_allocas.data[j]);
    }
    for (uint32_t j = 0u; j < arg_consts.len; ++j) {
        PIR_VEC_PUSH(&caller->blocks.data[caller_bid].insts, arg_consts.data[j]);
    }
    PIR_VEC_FREE(&kept_allocas);
    PIR_VEC_FREE(&arg_consts);

    /* 3. Pass 1: Allocate cloned blocks, block params, and instruction result ValueIds. */
    for (uint32_t ri = 0u; ri < ccfg->rpo_len; ++ri) {
        PirBlockId cb = ccfg->rpo[ri];
        const PirBlock *cblk = &callee->blocks.data[cb];
        PirBlockId nb = pir_fn_new_block(mod, caller, "inl_b", call_line);
        blk_map[cb] = nb;

        for (uint32_t pi = 0u; pi < cblk->params.len; ++pi) {
            PirValueId old_bp = cblk->params.data[pi];
            PirType bpty = callee->values.data[old_bp].type;
            PirValueId new_bp = pir_fn_new_val(mod, caller, "iv", bpty, nb, PIR_INVALID_ID, pi);
            PIR_VEC_PUSH(&caller->blocks.data[nb].params, new_bp);
            val_map[old_bp] = new_bp;
        }

        for (uint32_t j = 0u; j < cblk->insts.len; ++j) {
            const PirInst *ci = &callee->insts.data[cblk->insts.data[j]];
            if (ci->op == PIR_OP_RET) continue;
            for (uint32_t r = 0u; r < 2u; ++r) {
                if (ci->results[r] != PIR_INVALID_ID) {
                    PirType rty = callee->values.data[ci->results[r]].type;
                    val_map[ci->results[r]] = pir_fn_new_val(mod, caller, "iv", rty, nb, PIR_INVALID_ID, r);
                }
            }
        }
    }

    /* 4. Jump from caller_bid to cloned callee entry block. */
    {
        PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), call_line, call_loc);
        jmp.targets[0].target = blk_map[0];
        PirInstId jiid = caller->insts.len;
        PIR_VEC_PUSH(&caller->insts, jmp);
        PIR_VEC_PUSH(&caller->blocks.data[caller_bid].insts, jiid);
    }

    /* 5. Pass 2: Clone instructions, convert RET -> JMP cont_bid, hoist entry alloca.ptr. */
    PirInstIdVec hoisted_allocas = {0};
    for (uint32_t ri = 0u; ri < ccfg->rpo_len; ++ri) {
        PirBlockId cb = ccfg->rpo[ri];
        PirBlockId nb = blk_map[cb];
        const PirBlock *cblk = &callee->blocks.data[cb];

        for (uint32_t j = 0u; j < cblk->insts.len; ++j) {
            const PirInst *ci = &callee->insts.data[cblk->insts.data[j]];
            if (ci->op == PIR_OP_RET) {
                PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), call_line, call_loc);
                jmp.targets[0].target = cont_bid;
                if (ret_vid != PIR_INVALID_ID && ci->operand_count == 1u) {
                    jmp.targets[0].args = pir_alloc_ops(mod, 1u);
                    jmp.targets[0].args[0] = remap_operand(ci->operands[0], val_map, callee_nvals);
                    jmp.targets[0].arg_count = 1u;
                }
                PirInstId jiid = caller->insts.len;
                PIR_VEC_PUSH(&caller->insts, jmp);
                PIR_VEC_PUSH(&caller->blocks.data[nb].insts, jiid);
                continue;
            }

            PirInst ni = *ci;
            ni.loc_idx  = call_loc;
            ni.src_line = call_line;
            ni.operands = NULL;
            if (ci->operand_count > 0u) {
                ni.operands = pir_alloc_ops(mod, ci->operand_count);
                for (uint32_t k = 0u; k < ci->operand_count; ++k) {
                    ni.operands[k] = remap_operand(ci->operands[k], val_map, callee_nvals);
                }
            }
            if (ci->shuffle_len > 0u && ci->shuffle_mask) {
                ni.shuffle_mask = (uint8_t *)pir_arena_alloc(&mod->arena, ci->shuffle_len);
                memcpy(ni.shuffle_mask, ci->shuffle_mask, ci->shuffle_len);
            }
            memset(ni.targets, 0, sizeof(ni.targets));
            ni.targets[0].target = PIR_INVALID_ID;
            ni.targets[1].target = PIR_INVALID_ID;
            ni.switch_cases = NULL;
            ni.switch_case_count = 0u;

            if (ci->op == PIR_OP_JMP) {
                ni.targets[0] = clone_target(mod, &ci->targets[0], blk_map, callee_nblks, val_map, callee_nvals);
            } else if (ci->op == PIR_OP_BR) {
                ni.targets[0] = clone_target(mod, &ci->targets[0], blk_map, callee_nblks, val_map, callee_nvals);
                ni.targets[1] = clone_target(mod, &ci->targets[1], blk_map, callee_nblks, val_map, callee_nvals);
            } else if (ci->op == PIR_OP_SWITCH) {
                ni.targets[0] = clone_target(mod, &ci->targets[0], blk_map, callee_nblks, val_map, callee_nvals);
                ni.switch_case_count = ci->switch_case_count;
                if (ci->switch_case_count > 0u) {
                    ni.switch_cases = (PirSwitchCase *)pir_arena_alloc(
                        &mod->arena, (size_t)ci->switch_case_count * sizeof(PirSwitchCase));
                    for (uint32_t sc = 0u; sc < ci->switch_case_count; ++sc) {
                        ni.switch_cases[sc] = ci->switch_cases[sc];
                        ni.switch_cases[sc].target = clone_target(
                            mod, &ci->switch_cases[sc].target, blk_map, callee_nblks, val_map, callee_nvals);
                    }
                }
            }

            ni.results[0] = (ci->results[0] != PIR_INVALID_ID) ? val_map[ci->results[0]] : PIR_INVALID_ID;
            ni.results[1] = (ci->results[1] != PIR_INVALID_ID) ? val_map[ci->results[1]] : PIR_INVALID_ID;

            PirInstId new_iid = caller->insts.len;
            PIR_VEC_PUSH(&caller->insts, ni);

            if (ni.op == PIR_OP_ALLOCA_PTR) {
                PIR_VEC_PUSH(&hoisted_allocas, new_iid);
                if (ni.results[0] != PIR_INVALID_ID) {
                    caller->values.data[ni.results[0]].def_block = 0u;
                    caller->values.data[ni.results[0]].def_inst  = new_iid;
                }
            } else {
                PIR_VEC_PUSH(&caller->blocks.data[nb].insts, new_iid);
                for (uint32_t r = 0u; r < 2u; ++r) {
                    if (ni.results[r] != PIR_INVALID_ID) {
                        caller->values.data[ni.results[r]].def_block = nb;
                        caller->values.data[ni.results[r]].def_inst  = new_iid;
                    }
                }
            }
        }
    }

    if (hoisted_allocas.len > 0u) {
        PirBlock *eb = &caller->blocks.data[0];
        uint32_t old_len = eb->insts.len;
        uint32_t add_len = hoisted_allocas.len;
        for (uint32_t k = 0u; k < add_len; ++k) {
            PIR_VEC_PUSH(&eb->insts, 0u);
        }
        memmove(&eb->insts.data[add_len], &eb->insts.data[0],
                (size_t)old_len * sizeof(PirInstId));
        memcpy(&eb->insts.data[0], hoisted_allocas.data,
               (size_t)add_len * sizeof(PirInstId));
    }

    PIR_VEC_FREE(&hoisted_allocas);
    free(val_map);
    free(blk_map);
}

static void clone_func_snapshot(const PirFunction *src, PirFunction *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->name        = src->name;
    dst->linkage     = src->linkage;
    dst->is_defined  = src->is_defined;
    dst->is_variadic = src->is_variadic;
    dst->param_count = src->param_count;
    dst->params      = src->params;
    dst->ret_type    = src->ret_type;
    dst->cabi_ret    = src->cabi_ret;

    dst->blocks.len = src->blocks.len;
    dst->blocks.cap = src->blocks.len;
    if (src->blocks.len > 0u) {
        dst->blocks.data = (PirBlock *)calloc(src->blocks.len, sizeof(PirBlock));
        if (!dst->blocks.data) abort();
        for (uint32_t b = 0u; b < src->blocks.len; ++b) {
            dst->blocks.data[b].label    = src->blocks.data[b].label;
            dst->blocks.data[b].src_line = src->blocks.data[b].src_line;
            uint32_t np = src->blocks.data[b].params.len;
            if (np > 0u) {
                dst->blocks.data[b].params.data = (PirValueId *)malloc((size_t)np * sizeof(PirValueId));
                if (!dst->blocks.data[b].params.data) abort();
                memcpy(dst->blocks.data[b].params.data, src->blocks.data[b].params.data,
                       (size_t)np * sizeof(PirValueId));
                dst->blocks.data[b].params.len = np;
                dst->blocks.data[b].params.cap = np;
            }
            uint32_t ni = src->blocks.data[b].insts.len;
            if (ni > 0u) {
                dst->blocks.data[b].insts.data = (PirInstId *)malloc((size_t)ni * sizeof(PirInstId));
                if (!dst->blocks.data[b].insts.data) abort();
                memcpy(dst->blocks.data[b].insts.data, src->blocks.data[b].insts.data,
                       (size_t)ni * sizeof(PirInstId));
                dst->blocks.data[b].insts.len = ni;
                dst->blocks.data[b].insts.cap = ni;
            }
        }
    }

    dst->insts.len = src->insts.len;
    dst->insts.cap = src->insts.len;
    if (src->insts.len > 0u) {
        dst->insts.data = (PirInst *)malloc((size_t)src->insts.len * sizeof(PirInst));
        if (!dst->insts.data) abort();
        memcpy(dst->insts.data, src->insts.data, (size_t)src->insts.len * sizeof(PirInst));
    }

    dst->values.len = src->values.len;
    dst->values.cap = src->values.len;
    if (src->values.len > 0u) {
        dst->values.data = (PirValueDef *)malloc((size_t)src->values.len * sizeof(PirValueDef));
        if (!dst->values.data) abort();
        memcpy(dst->values.data, src->values.data, (size_t)src->values.len * sizeof(PirValueDef));
    }
}

static void free_func_snapshot(PirFunction *snap) {
    for (uint32_t b = 0u; b < snap->blocks.len; ++b) {
        free(snap->blocks.data[b].params.data);
        free(snap->blocks.data[b].insts.data);
    }
    free(snap->blocks.data);
    free(snap->insts.data);
    free(snap->values.data);
}

static bool func_calls_target(const PirModule *mod, const PirFunction *fn, uint32_t target_fi) {
    for (uint32_t bi = 0u; bi < fn->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[bi];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u &&
                find_func_by_sym(mod, &ins->operands[0]) == target_fi) {
                return true;
            }
        }
    }
    return false;
}

/* Bounded 1-level self-inlining for tiny pure-register recursive functions (e.g. Fib, Tak),
 * gated by target callee-saved register budget so we avoid spilling on 5-cs-reg x86-64. */
static void try_self_inline_tiny(PirModule *mod, PirFunction *fn, uint32_t fi,
                                 const PirTargetSpec *target) {
    uint32_t max_params = (target->arch == PIR_ARCH_ARM64) ? 3u : 2u;
    uint32_t max_calls  = (target->arch == PIR_ARCH_ARM64) ? 3u : 2u;
    if (!fn->is_defined || fn->linkage == PIR_LINK_WEAK || fn->is_variadic ||
        fn->cabi_ret.kind != PIR_CABI_RET_NONE || fn->blocks.len == 0u ||
        fn->blocks.data[0].params.len != 0u || fn->param_count > max_params) {
        return;
    }
    for (uint32_t k = 0u; k < fn->param_count; ++k) {
        if (fn->params[k].abi_byval_size > 0u ||
            fn->params[k].val_id == PIR_INVALID_ID || fn->params[k].val_id >= fn->values.len) {
            return;
        }
    }

    PirCfg ccfg;
    pir_cfg_build(&ccfg, fn);
    if (ccfg.rpo_len == 0u || ccfg.rpo_len > 12u) { pir_cfg_destroy(&ccfg); return; }

    uint32_t n_insts = 0u, n_rets = 0u, self_calls = 0u;
    bool ok = true;
    for (uint32_t ri = 0u; ri < ccfg.rpo_len && ok; ++ri) {
        const PirBlock *blk = &fn->blocks.data[ccfg.rpo[ri]];
        n_insts += blk->insts.len;
        if (n_insts > 36u) { ok = false; break; }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_ALLOCA_PTR || ins->op == PIR_OP_ALLOCA_DYN ||
                ins->op == PIR_OP_STACKSAVE  || ins->op == PIR_OP_STACKRESTORE ||
                ins->op == PIR_OP_VA_START   || ins->op == PIR_OP_CALL_INDIRECT ||
                ins->op == PIR_OP_INVOKE     || ins->op == PIR_OP_INVOKE_INDIRECT) {
                ok = false; break;
            }
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u) {
                uint32_t cid = find_func_by_sym(mod, &ins->operands[0]);
                if (cid == fi) {
                    if (!call_site_matches_callee(fn, ins, fn)) { ok = false; break; }
                    self_calls++;
                } else if (ins->operands[0].kind == PIR_OPER_SYM &&
                           pir_is_returns_twice_name(ins->operands[0].u.sym.name)) {
                    ok = false; break;
                }
            }
            if (ins->op == PIR_OP_RET) n_rets++;
        }
    }
    pir_cfg_destroy(&ccfg);
    if (!ok || n_rets == 0u || self_calls == 0u || self_calls > max_calls) return;

    PirFunction snap;
    clone_func_snapshot(fn, &snap);
    PirCfg scfg;
    pir_cfg_build(&scfg, &snap);

    /* Inline ONLY original reachable self-call sites (iid < orig_ninsts) for exact 1-level unfolding. */
    uint32_t orig_ninsts = fn->insts.len;
    uint32_t orig_nblks  = snap.blocks.len;
    uint32_t inlined = 0u;
    for (uint32_t bi = 0u; bi < fn->blocks.len && inlined < self_calls; ++bi) {
        if (bi < orig_nblks && !scfg.reachable[bi]) continue;
        for (uint32_t j = 0u; j < fn->blocks.data[bi].insts.len; ++j) {
            PirInstId iid = fn->blocks.data[bi].insts.data[j];
            if (iid >= orig_ninsts) continue;
            const PirInst *ins = &fn->insts.data[iid];
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u &&
                find_func_by_sym(mod, &ins->operands[0]) == fi) {
                inline_call_site(mod, fn, bi, j, &snap, &scfg);
                inlined++;
                break;
            }
        }
    }

    pir_cfg_destroy(&scfg);
    free_func_snapshot(&snap);
}

void pir_opt_inline(PirModule *mod, const PirTargetSpec *target) {
    uint32_t nfuncs = mod->funcs.len;
    if (nfuncs == 0u) return;

    /* Count direct calls and detect self-recursive functions across the module. */
    uint32_t *call_counts = (uint32_t *)calloc(nfuncs, sizeof(uint32_t));
    bool     *is_self_rec = (bool *)calloc(nfuncs, sizeof(bool));
    bool     *inlined_mut = (bool *)calloc(nfuncs, sizeof(bool));
    if (!call_counts || !is_self_rec || !inlined_mut) abort();

    for (uint32_t fi = 0u; fi < nfuncs; ++fi) {
        const PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;
        for (uint32_t bi = 0u; bi < fn->blocks.len; ++bi) {
            const PirBlock *blk = &fn->blocks.data[bi];
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
                if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u) {
                    uint32_t cid = find_func_by_sym(mod, &ins->operands[0]);
                    if (cid < nfuncs) {
                        call_counts[cid]++;
                        if (cid == fi) is_self_rec[fi] = true;
                    }
                }
            }
        }
    }

    for (uint32_t fi = 0u; fi < nfuncs; ++fi) {
        if (is_self_rec[fi]) {
            try_self_inline_tiny(mod, &mod->funcs.data[fi], fi, target);
        }
    }

    for (uint32_t fi = 0u; fi < nfuncs; ++fi) {
        PirFunction *caller = &mod->funcs.data[fi];
        if (!caller->is_defined || caller->blocks.len == 0u) continue;
        if (caller->insts.len >= INLINE_MAX_CALLER_INSTS) continue;

        memset(inlined_mut, 0, nfuncs * sizeof(bool));
        uint32_t added_insts = 0u;
        uint64_t added_alloca_bytes = 0u;
        for (uint32_t bi = 0u; bi < caller->blocks.len; ++bi) {
            if (caller->insts.len >= INLINE_MAX_CALLER_INSTS ||
                added_insts >= INLINE_MAX_ADDED_PER_CALLER) {
                break;
            }
            for (uint32_t j = 0u; j < caller->blocks.data[bi].insts.len; ++j) {
                PirInstId iid = caller->blocks.data[bi].insts.data[j];
                const PirInst *ins = &caller->insts.data[iid];
                if (ins->op != PIR_OP_CALL || ins->operand_count < 1u) continue;

                uint32_t cid = find_func_by_sym(mod, &ins->operands[0]);
                if (cid == PIR_INVALID_ID || cid == fi || is_self_rec[cid] || inlined_mut[cid]) {
                    continue;
                }

                const PirFunction *callee = &mod->funcs.data[cid];
                if (!call_site_matches_callee(caller, ins, callee)) continue;

                PirCfg ccfg;
                pir_cfg_build(&ccfg, callee);
                uint32_t callee_insts = 0u;
                uint64_t callee_alloca_bytes = 0u;
                bool ok = is_callee_inlineable(callee, &ccfg, call_counts[cid],
                                               &callee_insts, &callee_alloca_bytes);
                if (ok && is_self_rec[fi] && callee_alloca_bytes > 0u) {
                    ok = false;
                }
                if (ok && added_alloca_bytes + callee_alloca_bytes > 8192u) {
                    ok = false;
                }
                if (ok &&
                    caller->insts.len + callee_insts <= INLINE_MAX_CALLER_INSTS &&
                    added_insts + callee_insts <= INLINE_MAX_ADDED_PER_CALLER) {
                    bool mut_rec = func_calls_target(mod, callee, fi);
                    inline_call_site(mod, caller, bi, j, callee, &ccfg);
                    added_insts += callee_insts;
                    added_alloca_bytes += callee_alloca_bytes;
                    if (mut_rec) {
                        is_self_rec[fi] = true;
                        inlined_mut[cid] = true;
                    }
                    pir_cfg_destroy(&ccfg);
                    /* Remaining instructions of block bi moved to cont_bid (appended to blocks),
                     * which will be visited later in the outer block loop. */
                    break;
                }
                pir_cfg_destroy(&ccfg);
            }
        }
    }

    free(inlined_mut);
    free(is_self_rec);
    free(call_counts);
}
