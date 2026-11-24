/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/alias.h"
#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "analysis/loops.h"

static bool op_is_invariant(const PirFunction *fn, const PirLoop *lp, PirOperand op) {
    if (op.kind != PIR_OPER_VAL) return true;
    if (op.u.val >= fn->values.len) return false;
    PirBlockId db = fn->values.data[op.u.val].def_block;
    if (db == PIR_INVALID_ID) return true; /* Function parameter */
    if (db >= lp->num_blocks) return true;
    return !lp->in_loop[db];
}

static bool all_ops_invariant(const PirFunction *fn, const PirLoop *lp, const PirInst *ins) {
    for (uint32_t i = 0u; i < ins->operand_count; ++i) {
        if (!op_is_invariant(fn, lp, ins->operands[i])) return false;
    }
    return true;
}

static bool is_pure_hoistable_op(const PirInst *ins) {
    uint32_t trap_ub = PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U |
                       PIR_FLAG_UNSAFE | PIR_FLAG_NONZERO | PIR_FLAG_VOLATILE;
    if (ins->flags & trap_ub) return false;
    bool has_unsafe_int = (ins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u;
    if (has_unsafe_int &&
        ins->op != PIR_OP_ADD && ins->op != PIR_OP_SUB && ins->op != PIR_OP_MUL) {
        return false;
    }

    switch (ins->op) {
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: case PIR_OP_FNEG:
        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U:
        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE:
        case PIR_OP_SELECT:
        case PIR_OP_ZEXT: case PIR_OP_SEXT: case PIR_OP_TRUNC:
        case PIR_OP_SITOFP: case PIR_OP_UITOFP: case PIR_OP_FPEXT: case PIR_OP_FPTRUNC:
        case PIR_OP_BITCAST: case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_PTR_ADD: case PIR_OP_SPLAT: case PIR_OP_EXTRACT:
        case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
            return true;
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI:
            return (ins->flags & PIR_FLAG_SAT) != 0u;
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_STACKRESTORE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_LOAD: case PIR_OP_STORE:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_LOAD: case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW: case PIR_OP_ATOMIC_CMPXCHG: case PIR_OP_FENCE:
        case PIR_OP_VA_START: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_CALL: case PIR_OP_CALL_INDIRECT:
        case PIR_OP_JMP: case PIR_OP_BR: case PIR_OP_SWITCH:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            return false;
    }
    return false;
}

static bool loop_has_mem_barrier(const PirModule *mod, const PirFunction *fn, const PirLoop *lp) {
    for (uint32_t bi = 0u; bi < lp->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[lp->blocks.data[bi]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_CALL_INDIRECT || ins->op == PIR_OP_FENCE ||
                ins->op == PIR_OP_ATOMIC_LOAD || ins->op == PIR_OP_ATOMIC_STORE ||
                ins->op == PIR_OP_ATOMIC_RMW || ins->op == PIR_OP_ATOMIC_CMPXCHG ||
                ins->op == PIR_OP_VA_START || ins->op == PIR_OP_STACKRESTORE) {
                return true;
            }
            if (ins->op == PIR_OP_CALL) {
                bool is_ro = false;
                if (ins->operands[0].kind == PIR_OPER_SYM) {
                    const char *cn = ins->operands[0].u.sym.name;
                    bool is_def = (ins->operands[0].u.sym.is_func &&
                                   ins->operands[0].u.sym.id < mod->funcs.len &&
                                   mod->funcs.data[ins->operands[0].u.sym.id].is_defined);
                    if (ins->operands[0].u.sym.id < mod->funcs.len) {
                        is_ro = (mod->funcs.data[ins->operands[0].u.sym.id].attrs & PIR_FATTR_READONLY) != 0u;
                    }
                    if (!is_ro && !is_def && cn &&
                        (strcmp(cn, "sqrt") == 0 || strcmp(cn, "sqrtf") == 0 ||
                         strcmp(cn, "fabs") == 0 || strcmp(cn, "fabsf") == 0)) {
                        is_ro = true;
                    }
                }
                if (!is_ro) return true;
            }
        }
    }
    return false;
}

static bool loop_has_real_call(const PirFunction *fn, const PirLoop *lp) {
    for (uint32_t bi = 0u; bi < lp->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[lp->blocks.data[bi]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_CALL_INDIRECT) return true;
            if (ins->op == PIR_OP_CALL) {
                if (ins->operands[0].kind == PIR_OPER_SYM && ins->operands[0].u.sym.name) {
                    const char *cn = ins->operands[0].u.sym.name;
                    if (strcmp(cn, "sqrt") == 0 || strcmp(cn, "sqrtf") == 0 ||
                        strcmp(cn, "fabs") == 0 || strcmp(cn, "fabsf") == 0) {
                        continue;
                    }
                }
                return true;
            }
        }
    }
    return false;
}

static bool load_is_safe_to_hoist(const PirFunction *fn,
                                  const PirTargetSpec *target, const PirLoop *lp,
                                  PirBlockId b, const PirInst *ins, bool has_barrier) {
    if (ins->op != PIR_OP_LOAD || (ins->flags & PIR_FLAG_VOLATILE) || has_barrier) return false;
    if (!op_is_invariant(fn, lp, ins->operands[0])) return false;

    uint32_t lsz = pir_type_bits(ins->ty, target->ptr_bytes) / 8u;
    if (lsz == 0u) return false;

    /* Verify no store inside the loop may alias this load. */
    for (uint32_t bi = 0u; bi < lp->blocks.len; ++bi) {
        const PirBlock *blk = &fn->blocks.data[lp->blocks.data[bi]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *st = &fn->insts.data[blk->insts.data[j]];
            if (st->op == PIR_OP_STORE) {
                uint32_t ssz = pir_type_bits(st->ty, target->ptr_bytes) / 8u;
                if (pir_alias_query(fn, ins->operands[0], lsz, st->operands[1], ssz) != PIR_ALIAS_NO) {
                    return false;
                }
            }
        }
    }

    /* Dereference safety: header executes unconditionally, or base is known valid/noalias object. */
    if (b == lp->header) return true;
    PirPtrInfo pi = pir_alias_decompose(fn, ins->operands[0]);
    if (pi.kind == PIR_PTR_PARAM_NOALIAS || pi.kind == PIR_PTR_GLOBAL) return true;
    if (pi.kind == PIR_PTR_ALLOCA && pi.has_offset && pi.offset >= 0 &&
        pi.obj_id < fn->insts.len && fn->insts.data[pi.obj_id].op == PIR_OP_ALLOCA_PTR &&
        (uint64_t)pi.offset + (uint64_t)lsz <= fn->insts.data[pi.obj_id].imm_u64) {
        return true;
    }
    return false;
}

static void hoist_inst(PirFunction *fn, PirBlockId from_b, uint32_t idx, PirBlockId pre) {
    PirInstIdVec *src = &fn->blocks.data[from_b].insts;
    PirInstId iid = src->data[idx];
    for (uint32_t k = idx; k + 1u < src->len; ++k) {
        src->data[k] = src->data[k + 1u];
    }
    src->len--;

    /* Insert before preheader's terminator. */
    PirInstIdVec *dst = &fn->blocks.data[pre].insts;
    PIR_VEC_PUSH(dst, iid);
    if (dst->len >= 2u) {
        PirInstId term = dst->data[dst->len - 2u];
        dst->data[dst->len - 2u] = iid;
        dst->data[dst->len - 1u] = term;
    }

    PirInst *ins = &fn->insts.data[iid];
    if ((ins->op == PIR_OP_ADD || ins->op == PIR_OP_SUB || ins->op == PIR_OP_MUL) &&
        (ins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
        ins->flags &= ~(uint32_t)(PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U);
        ins->flags |= PIR_FLAG_WRAP;
    }
    if (ins->results[0] != PIR_INVALID_ID) fn->values.data[ins->results[0]].def_block = pre;
    if (ins->results[1] != PIR_INVALID_ID) fn->values.data[ins->results[1]].def_block = pre;
}

void pir_opt_licm(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len == 0u) return;

    for (uint32_t round = 0u; round < 64u; ++round) {
        PirCfg cfg;
        PirDom dom;
        PirLoopInfo li;
        pir_cfg_build(&cfg, fn);
        pir_dom_build(&dom, &cfg);
        pir_loops_build(&li, &cfg, &dom);

        bool rebuilt_needed = false;
        bool hoisted_any    = false;

        /* Sort loop indices innermost-first (smallest block count first). */
        uint32_t nl = li.loops.len;
        uint32_t *order = nl ? (uint32_t *)malloc(nl * sizeof(uint32_t)) : NULL;
        for (uint32_t i = 0u; i < nl; ++i) order[i] = i;
        for (uint32_t i = 0u; i < nl; ++i) {
            for (uint32_t j = i + 1u; j < nl; ++j) {
                if (li.loops.data[order[j]].blocks.len < li.loops.data[order[i]].blocks.len) {
                    uint32_t tmp = order[i];
                    order[i] = order[j];
                    order[j] = tmp;
                }
            }
        }

        for (uint32_t oi = 0u; oi < nl && !rebuilt_needed; ++oi) {
            PirLoop *lp = &li.loops.data[order[oi]];
            bool has_barrier = loop_has_mem_barrier(mod, fn, lp);
            bool has_call    = loop_has_real_call(fn, lp);

            bool loop_changed = true;
            while (loop_changed) {
                loop_changed = false;
                for (uint32_t ri = 0u; ri < cfg.rpo_len; ++ri) {
                    PirBlockId b = cfg.rpo[ri];
                    if (b >= lp->num_blocks || !lp->in_loop[b]) continue;

                    uint32_t j = 0u;
                    while (j + 1u < fn->blocks.data[b].insts.len) {
                        PirInstId iid = fn->blocks.data[b].insts.data[j];
                        const PirInst *ins = &fn->insts.data[iid];
                        bool can_hoist = false;

                        if (is_pure_hoistable_op(ins) && all_ops_invariant(fn, lp, ins)) {
                            /* 128-bit vector regs are caller-saved on AAPCS64 and SysV x64, and
                             * trivial constants are cheaper to rematerialize than to hold in a
                             * callee-saved register or spill slot across a call. */
                            if (!(has_call && (ins->ty.kind == PIR_TYPE_VECTOR || ins->op == PIR_OP_CONST))) {
                                can_hoist = true;
                            }
                        } else if (load_is_safe_to_hoist(fn, target, lp, b, ins, has_barrier)) {
                            can_hoist = true;
                        }

                        if (can_hoist) {
                            uint32_t prev_nblks = fn->blocks.len;
                            PirBlockId pre = pir_loop_ensure_preheader(mod, fn, &cfg, lp);
                            if (pre == PIR_INVALID_ID) {
                                j++;
                                continue;
                            }
                            hoist_inst(fn, b, j, pre);
                            hoisted_any  = true;
                            loop_changed = true;
                            if (fn->blocks.len != prev_nblks) {
                                /* A new preheader block was inserted; rebuild CFG/dom/loops. */
                                rebuilt_needed = true;
                                break;
                            }
                            continue;
                        }
                        j++;
                    }
                    if (rebuilt_needed) break;
                }
                if (rebuilt_needed) break;
            }
        }

        free(order);
        pir_loops_destroy(&li);
        pir_dom_destroy(&dom);
        pir_cfg_destroy(&cfg);

        if (!rebuilt_needed && !hoisted_any) break;
    }
}
