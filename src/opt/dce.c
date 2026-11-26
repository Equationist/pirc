/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "lower/lower.h"

static bool inst_is_live_root(const PirInst *ins) {
    uint32_t trap_ub = PIR_FLAG_TRAP | PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U |
                       PIR_FLAG_UNSAFE | PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U;
    switch (ins->op) {
        case PIR_OP_JMP:
        case PIR_OP_BR:
        case PIR_OP_SWITCH:
        case PIR_OP_INVOKE:
        case PIR_OP_INVOKE_INDIRECT:
        case PIR_OP_RESUME:
        case PIR_OP_RET:
        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
        case PIR_OP_STORE:
        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET:
        case PIR_OP_ATOMIC_LOAD:
        case PIR_OP_ATOMIC_STORE:
        case PIR_OP_ATOMIC_RMW:
        case PIR_OP_ATOMIC_CMPXCHG:
        case PIR_OP_FENCE:
        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
        case PIR_OP_VA_START:
        case PIR_OP_VA_ARG:
        case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY:
        case PIR_OP_VA_END:
        case PIR_OP_STACKRESTORE:
        case PIR_OP_ALLOCA_DYN:
        case PIR_OP_DIV_S:
        case PIR_OP_DIV_U:
        case PIR_OP_REM_S:
        case PIR_OP_REM_U:
            return true;
        case PIR_OP_LOAD:
            return (ins->flags & PIR_FLAG_VOLATILE) != 0u;
        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI:
            return (ins->flags & PIR_FLAG_SAT) == 0u;
        case PIR_OP_CLZ:
        case PIR_OP_CTZ:
            return (ins->flags & PIR_FLAG_NONZERO) != 0u;
        case PIR_OP_ADD:
        case PIR_OP_SUB:
        case PIR_OP_MUL:
        case PIR_OP_SHL:
        case PIR_OP_SHR:
        case PIR_OP_SAR:
            return (ins->flags & trap_ub) != 0u;
        case PIR_OP_CONST: case PIR_OP_ADDR:
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_MUL_WIDE_U: case PIR_OP_MUL_WIDE_S:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR:
        case PIR_OP_POPCNT: case PIR_OP_BSWAP:
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
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_ALLOCA_PTR: case PIR_OP_STACKSAVE:
        case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR: case PIR_OP_PTR_ADD:
            return false;
    }
    return false;
}

typedef struct {
    PirFunction  *fn;
    const PirCfg *cfg;
    bool         *inst_live;
    bool         *val_live;
    PirValueIdVec val_wl;
} DceCtx;

static void mark_val(DceCtx *ctx, PirValueId v) {
    if (v < ctx->fn->values.len && !ctx->val_live[v]) {
        ctx->val_live[v] = true;
        PIR_VEC_PUSH(&ctx->val_wl, v);
    }
}

static void mark_inst(DceCtx *ctx, PirInstId iid) {
    if (iid >= ctx->fn->insts.len || ctx->inst_live[iid]) return;
    ctx->inst_live[iid] = true;
    const PirInst *ins = &ctx->fn->insts.data[iid];
    for (uint32_t i = 0u; i < ins->operand_count; ++i) {
        if (ins->operands[i].kind == PIR_OPER_VAL) {
            mark_val(ctx, ins->operands[i].u.val);
        }
    }
}

static void mark_edge_arg(DceCtx *ctx, const PirBranchTarget *bt, PirBlockId dest, uint32_t pidx) {
    if (bt->target == dest && pidx < bt->arg_count && bt->args[pidx].kind == PIR_OPER_VAL) {
        mark_val(ctx, bt->args[pidx].u.val);
    }
}

static void compact_target_args(PirModule *mod, PirBranchTarget *bt,
                                const bool *val_live, const bool *reachable,
                                const PirValueIdVec *orig_params,
                                uint32_t nblks) {
    if (bt->target >= nblks || !reachable[bt->target] || bt->arg_count == 0u) return;
    const PirValueIdVec *op = &orig_params[bt->target];
    uint32_t keep_cnt = 0u;
    for (uint32_t k = 0u; k < op->len && k < bt->arg_count; ++k) {
        if (val_live[op->data[k]]) keep_cnt++;
    }
    if (keep_cnt == bt->arg_count) return;
    if (keep_cnt == 0u) {
        bt->args = NULL;
        bt->arg_count = 0u;
        return;
    }
    PirOperand *nargs = pir_alloc_ops(mod, keep_cnt);
    uint32_t dst = 0u;
    for (uint32_t k = 0u; k < op->len && k < bt->arg_count; ++k) {
        if (val_live[op->data[k]]) {
            nargs[dst++] = bt->args[k];
        }
    }
    bt->args = nargs;
    bt->arg_count = dst;
}

void pir_opt_dce(PirModule *mod, PirFunction *fn) {
    if (fn->blocks.len == 0u) return;

    PirCfg cfg;
    pir_cfg_build(&cfg, fn);

    DceCtx ctx;
    ctx.fn        = fn;
    ctx.cfg       = &cfg;
    ctx.inst_live = (bool *)calloc(fn->insts.len + 1u, sizeof(bool));
    ctx.val_live  = (bool *)calloc(fn->values.len + 1u, sizeof(bool));
    ctx.val_wl.data = NULL; ctx.val_wl.len = 0u; ctx.val_wl.cap = 0u;
    if (!ctx.inst_live || !ctx.val_live) abort();

    /* 1. Seed with side-effecting / trapping / terminator roots in reachable blocks.
     * Also preserve any reachable definitions still referenced by unreachable blocks
     * so the IR stays verifier-clean if unreachable blocks remain before simplify_cfg. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        if (!cfg.reachable[b]) {
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
                for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                    if (ins->operands[k].kind == PIR_OPER_VAL) mark_val(&ctx, ins->operands[k].u.val);
                }
                for (uint32_t t = 0u; t < 2u; ++t) {
                    for (uint32_t k = 0u; k < ins->targets[t].arg_count; ++k) {
                        if (ins->targets[t].args[k].kind == PIR_OPER_VAL) {
                            mark_val(&ctx, ins->targets[t].args[k].u.val);
                        }
                    }
                }
                for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                    for (uint32_t k = 0u; k < ins->switch_cases[c].target.arg_count; ++k) {
                        if (ins->switch_cases[c].target.args[k].kind == PIR_OPER_VAL) {
                            mark_val(&ctx, ins->switch_cases[c].target.args[k].u.val);
                        }
                    }
                }
            }
            continue;
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            if (inst_is_live_root(&fn->insts.data[iid])) {
                mark_inst(&ctx, iid);
            }
        }
    }

    /* 2. Propagate liveness backwards through SSA defs and block-param edges. */
    while (ctx.val_wl.len > 0u) {
        PirValueId v = ctx.val_wl.data[--ctx.val_wl.len];
        const PirValueDef *vd = &fn->values.data[v];
        if (vd->def_block == PIR_INVALID_ID) continue; /* Function param */
        if (vd->def_inst != PIR_INVALID_ID) {
            mark_inst(&ctx, vd->def_inst);
        } else if (vd->def_block < fn->blocks.len && cfg.reachable[vd->def_block]) {
            PirBlockId db = vd->def_block;
            uint32_t pidx = PIR_INVALID_ID;
            const PirBlock *dblk = &fn->blocks.data[db];
            for (uint32_t k = 0u; k < dblk->params.len; ++k) {
                if (dblk->params.data[k] == v) { pidx = k; break; }
            }
            if (pidx == PIR_INVALID_ID) continue;
            for (uint32_t pi = 0u; pi < cfg.preds[db].len; ++pi) {
                PirBlockId p = cfg.preds[db].data[pi];
                const PirBlock *pb = &fn->blocks.data[p];
                if (pb->insts.len == 0u) continue;
                const PirInst *term = &fn->insts.data[pb->insts.data[pb->insts.len - 1u]];
                mark_edge_arg(&ctx, &term->targets[0], db, pidx);
                mark_edge_arg(&ctx, &term->targets[1], db, pidx);
                for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                    mark_edge_arg(&ctx, &term->switch_cases[c].target, db, pidx);
                }
            }
        }
    }

    /* 3. Snapshot original block params, sweep dead insts & unused block params. */
    uint32_t nblks = fn->blocks.len;
    PirValueIdVec *orig_params = (PirValueIdVec *)calloc(nblks, sizeof(PirValueIdVec));
    if (!orig_params) abort();

    for (uint32_t b = 0u; b < nblks; ++b) {
        PirBlock *blk = &fn->blocks.data[b];
        if (!cfg.reachable[b]) continue;

        /* Copy original param list for edge arg compaction */
        for (uint32_t k = 0u; k < blk->params.len; ++k) {
            PIR_VEC_PUSH(&orig_params[b], blk->params.data[k]);
        }
        uint32_t pout = 0u;
        for (uint32_t k = 0u; k < blk->params.len; ++k) {
            PirValueId pv = blk->params.data[k];
            if (ctx.val_live[pv]) {
                fn->values.data[pv].param_index = pout;
                blk->params.data[pout++] = pv;
            }
        }
        blk->params.len = pout;

        uint32_t iout = 0u;
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            if (ctx.inst_live[iid]) {
                blk->insts.data[iout++] = iid;
            }
        }
        blk->insts.len = iout;
    }

    /* 4. Compact terminator argument lists (including from unreachable blocks targeting
     * reachable blocks) to match surviving block parameters. */
    for (uint32_t b = 0u; b < nblks; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) continue;
        PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
        if (term->op == PIR_OP_JMP) {
            compact_target_args(mod, &term->targets[0], ctx.val_live, cfg.reachable, orig_params, nblks);
        } else if (term->op == PIR_OP_BR) {
            compact_target_args(mod, &term->targets[0], ctx.val_live, cfg.reachable, orig_params, nblks);
            compact_target_args(mod, &term->targets[1], ctx.val_live, cfg.reachable, orig_params, nblks);
        } else if (term->op == PIR_OP_SWITCH) {
            compact_target_args(mod, &term->targets[0], ctx.val_live, cfg.reachable, orig_params, nblks);
            for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                compact_target_args(mod, &term->switch_cases[c].target,
                                    ctx.val_live, cfg.reachable, orig_params, nblks);
            }
        }
    }

    for (uint32_t b = 0u; b < nblks; ++b) PIR_VEC_FREE(&orig_params[b]);
    free(orig_params);
    PIR_VEC_FREE(&ctx.val_wl);
    free(ctx.val_live);
    free(ctx.inst_live);
    pir_cfg_destroy(&cfg);
}
