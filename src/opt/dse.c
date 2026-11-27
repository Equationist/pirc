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
#include "lower/lower.h"

typedef struct {
    uint32_t   blk_inst_idx;
    PirOperand ptr;
    PirOperand val;
    PirType    ty;
    uint32_t   size;
    bool       observed;
} ActiveStore;

typedef struct {
    PirOperand ptr;
    PirValueId val;
    PirType    ty;
    uint32_t   size;
} ActiveLoad;

typedef PIR_VEC(ActiveStore) ActiveStoreVec;
typedef PIR_VEC(ActiveLoad)  ActiveLoadVec;

static PirValueId resolve_alias(PirValueId *alias, uint32_t nvals, PirValueId v) {
    PirValueId root = v;
    uint32_t steps = 0u;
    while (root < nvals && alias[root] != PIR_INVALID_ID && steps < nvals) {
        root = alias[root];
        steps++;
    }
    PirValueId cur = v;
    while (cur != root && cur < nvals && alias[cur] != PIR_INVALID_ID) {
        PirValueId next = alias[cur];
        alias[cur] = root;
        cur = next;
    }
    return root;
}

static void resolve_ops(PirValueId *alias, uint32_t nvals, PirOperand *ops, uint32_t n) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL) {
            ops[i].u.val = resolve_alias(alias, nvals, ops[i].u.val);
        }
    }
}

static bool is_mem_barrier(PirOpcode op) {
    switch (op) {
        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
        case PIR_OP_INVOKE:
        case PIR_OP_INVOKE_INDIRECT:
        case PIR_OP_RESUME:
        case PIR_OP_ATOMIC_LOAD:
        case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW:
        case PIR_OP_ATOMIC_CMPXCHG:
        case PIR_OP_FENCE:
        case PIR_OP_VA_START:
        case PIR_OP_VA_ARG:
        case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY:
        case PIR_OP_VA_END:
        case PIR_OP_STACKRESTORE:
        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET:
            return true;
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD: case PIR_OP_SUB: case PIR_OP_MUL:
        case PIR_OP_DIV_S: case PIR_OP_DIV_U: case PIR_OP_REM_S: case PIR_OP_REM_U:
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
        case PIR_OP_FPTOSI: case PIR_OP_FPTOUI: case PIR_OP_SITOFP: case PIR_OP_UITOFP:
        case PIR_OP_FPEXT: case PIR_OP_FPTRUNC: case PIR_OP_BITCAST:
        case PIR_OP_PTRTOINT: case PIR_OP_INTTOPTR:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_ALLOCA_DYN:
        case PIR_OP_STACKSAVE: case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_PTR_ADD: case PIR_OP_LOAD: case PIR_OP_STORE:
        case PIR_OP_JMP: case PIR_OP_BR: case PIR_OP_SWITCH:
        case PIR_OP_RET: case PIR_OP_TRAP: case PIR_OP_UNREACHABLE:
            return false;
    }
    return false;
}

typedef struct {
    PirModule           *mod;
    PirFunction         *fn;
    const PirTargetSpec *target;
    const PirCfg        *cfg;
    const PirDom        *dom;
    PirValueId          *alias;
    uint32_t             nvals;
} DseCtx;

static void dse_process_block(DseCtx *ctx, PirBlockId b, const ActiveLoadVec *parent_loads,
                              ActiveLoadVec *out_loads) {
    PirFunction *fn = ctx->fn;
    PirBlock *blk = &fn->blocks.data[b];
    uint32_t n = blk->insts.len;

    ActiveStoreVec active = {NULL, 0u, 0u};
    ActiveLoadVec  loads  = {NULL, 0u, 0u};

    /* Inherit active loads ONLY when b has a single predecessor (strictly dominated with
     * zero alternate paths that could clobber memory). */
    if (parent_loads && parent_loads->len > 0u && ctx->cfg->preds[b].len == 1u) {
        for (uint32_t k = 0u; k < parent_loads->len; ++k) {
            PIR_VEC_PUSH(&loads, parent_loads->data[k]);
        }
    }

    if (n > 0u) {
        bool *dead = (bool *)calloc(n, sizeof(bool));
        if (!dead) abort();

        for (uint32_t j = 0u; j < n; ++j) {
            PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            resolve_ops(ctx->alias, ctx->nvals, ins->operands, ins->operand_count);

            if (ins->op == PIR_OP_STORE) {
                if (ins->flags & PIR_FLAG_VOLATILE) {
                    active.len = 0u;
                    loads.len  = 0u;
                    continue;
                }
                uint32_t ssz = pir_type_bits(ins->ty, ctx->target->ptr_bytes) / 8u;
                uint32_t dst = 0u;
                for (uint32_t k = 0u; k < active.len; ++k) {
                    PirAliasResult ar = pir_alias_query(fn, active.data[k].ptr, active.data[k].size,
                                                        ins->operands[1], ssz, ctx->target->ptr_bytes);
                    if (ar == PIR_ALIAS_MUST) {
                        if (!active.data[k].observed && active.data[k].size == ssz) {
                            dead[active.data[k].blk_inst_idx] = true;
                        }
                    } else if (ar == PIR_ALIAS_MAY) {
                        /* Partial/unknown overlap: invalidate for forwarding without deleting. */
                    } else {
                        active.data[dst++] = active.data[k];
                    }
                }
                active.len = dst;

                uint32_t ldst = 0u;
                for (uint32_t k = 0u; k < loads.len; ++k) {
                    PirAliasResult ar = pir_alias_query(fn, loads.data[k].ptr, loads.data[k].size,
                                                        ins->operands[1], ssz, ctx->target->ptr_bytes);
                    if (ar == PIR_ALIAS_NO) {
                        loads.data[ldst++] = loads.data[k];
                    }
                }
                loads.len = ldst;

                ActiveStore as;
                as.blk_inst_idx = j;
                as.ptr          = ins->operands[1];
                as.val          = ins->operands[0];
                as.ty           = ins->ty;
                as.size         = ssz;
                as.observed     = false;
                PIR_VEC_PUSH(&active, as);
            } else if (ins->op == PIR_OP_LOAD) {
                if (ins->flags & PIR_FLAG_VOLATILE) {
                    active.len = 0u;
                    loads.len  = 0u;
                    continue;
                }
                uint32_t lsz = pir_type_bits(ins->ty, ctx->target->ptr_bytes) / 8u;
                bool forwarded = false;
                for (uint32_t k = active.len; k > 0u; --k) {
                    ActiveStore *as = &active.data[k - 1u];
                    PirAliasResult ar = pir_alias_query(fn, as->ptr, as->size, ins->operands[0], lsz,
                                                        ctx->target->ptr_bytes);
                    if (ar == PIR_ALIAS_MUST && pir_type_eq(as->ty, ins->ty)) {
                        if (as->val.kind == PIR_OPER_VAL) {
                            ctx->alias[ins->results[0]] = resolve_alias(ctx->alias, ctx->nvals, as->val.u.val);
                            dead[j] = true;
                        } else {
                            PirOpcode cop = (as->val.kind == PIR_OPER_SYM) ? PIR_OP_ADDR : PIR_OP_CONST;
                            ins->op            = cop;
                            ins->flags         = PIR_FLAG_NONE;
                            ins->align         = 0u;
                            ins->operands      = pir_alloc_ops(ctx->mod, 1u);
                            ins->operands[0]   = as->val;
                            ins->operand_count = 1u;
                        }
                        forwarded = true;
                        break;
                    }
                    if (ar != PIR_ALIAS_NO) {
                        as->observed = true;
                    }
                }
                if (!forwarded) {
                    for (uint32_t k = loads.len; k > 0u; --k) {
                        ActiveLoad *al = &loads.data[k - 1u];
                        PirAliasResult ar = pir_alias_query(fn, al->ptr, al->size, ins->operands[0], lsz,
                                                            ctx->target->ptr_bytes);
                        if (ar == PIR_ALIAS_MUST && pir_type_eq(al->ty, ins->ty)) {
                            ctx->alias[ins->results[0]] = resolve_alias(ctx->alias, ctx->nvals, al->val);
                            dead[j] = true;
                            forwarded = true;
                            break;
                        }
                    }
                }
                if (!forwarded) {
                    for (uint32_t k = 0u; k < active.len; ++k) {
                        PirAliasResult ar = pir_alias_query(fn, active.data[k].ptr, active.data[k].size,
                                                            ins->operands[0], lsz, ctx->target->ptr_bytes);
                        if (ar != PIR_ALIAS_NO) active.data[k].observed = true;
                    }
                    ActiveLoad al;
                    al.ptr  = ins->operands[0];
                    al.val  = ins->results[0];
                    al.ty   = ins->ty;
                    al.size = lsz;
                    PIR_VEC_PUSH(&loads, al);
                }
            } else if (is_mem_barrier(ins->op) && !pir_inst_is_pure_math_call(ctx->mod, fn, ins)) {
                active.len = 0u;
                loads.len  = 0u;
            } else if (ins->op == PIR_OP_TRAP ||
                       (ins->flags & (PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u ||
                       ((ins->op == PIR_OP_DIV_S || ins->op == PIR_OP_DIV_U ||
                         ins->op == PIR_OP_REM_S || ins->op == PIR_OP_REM_U) &&
                        (ins->flags & PIR_FLAG_UNSAFE) == 0u)) {
                /* Deterministic traps may be observed by signal/trap handlers; do not delete
                 * preceding stores that must be visible before the trap fires. */
                for (uint32_t k = 0u; k < active.len; ++k) {
                    active.data[k].observed = true;
                }
            }
        }

        uint32_t out = 0u;
        for (uint32_t j = 0u; j < n; ++j) {
            if (!dead[j]) {
                blk->insts.data[out++] = blk->insts.data[j];
            }
        }
        blk->insts.len = out;
        free(dead);
    }

    PIR_VEC_FREE(&active);
    *out_loads = loads;
}

static void dse_walk_dom_tree(DseCtx *ctx, PirBlockId root_b) {
    typedef struct {
        PirBlockId    b;
        uint32_t      next_child;
        bool          entered;
        ActiveLoadVec loads;
    } DseFrame;
    PIR_VEC(DseFrame) stk = {0};
    DseFrame init;
    memset(&init, 0, sizeof(init));
    init.b = root_b;
    PIR_VEC_PUSH(&stk, init);

    while (stk.len > 0u) {
        DseFrame *fr = &stk.data[stk.len - 1u];
        PirBlockId b = fr->b;
        if (!fr->entered) {
            fr->entered = true;
            const ActiveLoadVec *pl = (stk.len >= 2u) ? &stk.data[stk.len - 2u].loads : NULL;
            dse_process_block(ctx, b, pl, &fr->loads);
        }
        if (fr->next_child < ctx->dom->children[b].len) {
            PirBlockId child = ctx->dom->children[b].data[fr->next_child++];
            DseFrame cf;
            memset(&cf, 0, sizeof(cf));
            cf.b = child;
            PIR_VEC_PUSH(&stk, cf);
        } else {
            PIR_VEC_FREE(&fr->loads);
            stk.len--;
        }
    }
    PIR_VEC_FREE(&stk);
}

void pir_opt_dse(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len == 0u || fn->values.len == 0u) return;

    PirCfg cfg;
    PirDom dom;
    pir_cfg_build(&cfg, fn);
    pir_dom_build(&dom, &cfg);

    uint32_t nvals = fn->values.len;
    PirValueId *alias = (PirValueId *)malloc(nvals * sizeof(PirValueId));
    if (!alias) abort();
    for (uint32_t v = 0u; v < nvals; ++v) alias[v] = PIR_INVALID_ID;

    DseCtx ctx;
    ctx.mod    = mod;
    ctx.fn     = fn;
    ctx.target = target;
    ctx.cfg    = &cfg;
    ctx.dom    = &dom;
    ctx.alias  = alias;
    ctx.nvals  = nvals;

    dse_walk_dom_tree(&ctx, 0u);

    /* Apply forwarded load aliases across all blocks. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            resolve_ops(alias, nvals, ins->operands, ins->operand_count);
            resolve_ops(alias, nvals, ins->targets[0].args, ins->targets[0].arg_count);
            resolve_ops(alias, nvals, ins->targets[1].args, ins->targets[1].arg_count);
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                resolve_ops(alias, nvals, ins->switch_cases[c].target.args,
                            ins->switch_cases[c].target.arg_count);
            }
        }
    }

    free(alias);
    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
}
