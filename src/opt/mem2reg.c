/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "opt/opt.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "analysis/dom.h"
#include "lower/lower.h"

typedef struct {
    PirInstId   alloca_inst;
    PirValueId  alloca_val;
    uint64_t    alloc_size;
    PirType     slot_type;
    bool        has_type;
    bool        promotable;
    PirValueId  zero_val;
} Mem2RegSlot;

static uint32_t min_type_bytes(PirType ty, uint8_t ptr_bytes) {
    uint32_t bits = pir_type_bits(ty, ptr_bytes);
    return bits / 8u;
}

static void disqualify_in_ops(const PirOperand *ops, uint32_t n,
                              const int32_t *val_to_slot, uint32_t nvals,
                              Mem2RegSlot *slots) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL && ops[i].u.val < nvals) {
            int32_t si = val_to_slot[ops[i].u.val];
            if (si >= 0) slots[si].promotable = false;
        }
    }
}

typedef struct {
    PirModule     *mod;
    PirFunction   *fn;
    const PirDom  *dom;
    Mem2RegSlot   *slots;
    uint32_t       nslots;
    uint32_t       nblks;
    uint32_t       orig_nvals;
    int32_t       *val_to_slot;
    PirValueId    *phi_param;   /* [nslots * nblks] */
    PirValueId    *val_remap;   /* [orig_nvals] */
    PirValueIdVec *stacks;      /* [nslots] */
    PirInstIdVec   entry_zeros; /* Deferred zero consts to prepend to entry block */
} RenameCtx;

static PirValueId resolve_val(RenameCtx *ctx, PirValueId v) {
    PirValueId root = v;
    uint32_t steps = 0u;
    while (root < ctx->orig_nvals && ctx->val_remap[root] != PIR_INVALID_ID &&
           steps < ctx->orig_nvals) {
        root = ctx->val_remap[root];
        steps++;
    }
    PirValueId cur = v;
    while (cur != root && cur < ctx->orig_nvals && ctx->val_remap[cur] != PIR_INVALID_ID) {
        PirValueId next = ctx->val_remap[cur];
        ctx->val_remap[cur] = root;
        cur = next;
    }
    return root;
}

static void remap_ops(RenameCtx *ctx, PirOperand *ops, uint32_t n) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL) {
            ops[i].u.val = resolve_val(ctx, ops[i].u.val);
        }
    }
}

static PirValueId get_or_make_zero(RenameCtx *ctx, uint32_t s) {
    if (ctx->slots[s].zero_val != PIR_INVALID_ID) {
        return ctx->slots[s].zero_val;
    }
    PirType ty = ctx->slots[s].slot_type;
    uint32_t ln = ctx->fn->blocks.data[0].src_line;
    PirValueId zv;
    if (ty.kind == PIR_TYPE_VECTOR) {
        PirType ety = pir_type_scalar(pir_vec_elem_scalar(ty.u.vec));
        PirValueId sv = pir_fn_new_val(ctx->mod, ctx->fn, "m2r_ez", ety, 0u, PIR_INVALID_ID, 0u);
        PirInst ci = pir_make_inst(PIR_OP_CONST, ety, ln, 0u);
        ci.operands = pir_alloc_ops(ctx->mod, 1u);
        ci.operand_count = 1u;
        if (pir_type_is_float(ety)) {
            ci.operands[0] = pir_op_imm_f(0u, ety);
        } else {
            ci.operands[0] = pir_op_imm_i(0u, false, ety);
        }
        ci.results[0] = sv;
        PirInstId cid0 = pir_fn_append_inst(ctx->fn, NULL, ci);
        PIR_VEC_PUSH(&ctx->entry_zeros, cid0);

        zv = pir_fn_new_val(ctx->mod, ctx->fn, "m2r_z", ty, 0u, PIR_INVALID_ID, 0u);
        PirInst sp = pir_make_inst(PIR_OP_SPLAT, ty, ln, 0u);
        sp.operands = pir_alloc_ops(ctx->mod, 1u);
        sp.operand_count = 1u;
        sp.operands[0] = pir_op_val(sv, ety);
        sp.results[0] = zv;
        PirInstId cid1 = pir_fn_append_inst(ctx->fn, NULL, sp);
        PIR_VEC_PUSH(&ctx->entry_zeros, cid1);
    } else {
        zv = pir_fn_new_val(ctx->mod, ctx->fn, "m2r_z", ty, 0u, PIR_INVALID_ID, 0u);
        PirInst ci = pir_make_inst(PIR_OP_CONST, ty, ln, 0u);
        ci.operands = pir_alloc_ops(ctx->mod, 1u);
        ci.operand_count = 1u;
        if (pir_type_is_float(ty)) {
            ci.operands[0] = pir_op_imm_f(0u, ty);
        } else {
            ci.operands[0] = pir_op_imm_i(0u, false, ty);
        }
        ci.results[0] = zv;
        PirInstId cid = pir_fn_append_inst(ctx->fn, NULL, ci);

        /* Record to prepend to blocks[0].insts after rename_block finishes so we never
         * mutate blocks[0].insts while rename_block(ctx, 0) is iterating over it. */
        PIR_VEC_PUSH(&ctx->entry_zeros, cid);
    }

    ctx->slots[s].zero_val = zv;
    return zv;
}

static PirValueId stack_top_or_zero(RenameCtx *ctx, uint32_t s) {
    if (ctx->stacks[s].len > 0u) {
        return ctx->stacks[s].data[ctx->stacks[s].len - 1u];
    }
    return get_or_make_zero(ctx, s);
}

static void append_phi_args_to_target(RenameCtx *ctx, PirInstId tid, int target_kind, uint32_t idx) {
    PirBranchTarget *bt_init = (target_kind == 0) ? &ctx->fn->insts.data[tid].targets[idx]
                                                   : &ctx->fn->insts.data[tid].switch_cases[idx].target;
    PirBlockId succ = bt_init->target;
    if (succ >= ctx->nblks) return;
    uint32_t extra = 0u;
    for (uint32_t s = 0u; s < ctx->nslots; ++s) {
        if (ctx->phi_param[(size_t)s * ctx->nblks + succ] != PIR_INVALID_ID) extra++;
    }
    if (extra == 0u) return;

    uint32_t orig_cnt = bt_init->arg_count;
    PirOperand *orig_args = bt_init->args;
    PirOperand *nargs = pir_alloc_ops(ctx->mod, orig_cnt + extra);
    for (uint32_t i = 0u; i < orig_cnt; ++i) {
        nargs[i] = orig_args[i];
    }
    uint32_t pos = orig_cnt;
    for (uint32_t s = 0u; s < ctx->nslots; ++s) {
        if (ctx->phi_param[(size_t)s * ctx->nblks + succ] != PIR_INVALID_ID) {
            PirValueId v = stack_top_or_zero(ctx, s);
            nargs[pos++] = pir_op_val(v, ctx->slots[s].slot_type);
        }
    }
    /* Re-index fn->insts.data in case stack_top_or_zero grew fn->insts. */
    PirBranchTarget *bt = (target_kind == 0) ? &ctx->fn->insts.data[tid].targets[idx]
                                             : &ctx->fn->insts.data[tid].switch_cases[idx].target;
    bt->args = nargs;
    bt->arg_count = pos;
}

static void rename_block(RenameCtx *ctx, PirBlockId root_b) {
    typedef struct {
        PirBlockId b;
        uint32_t   next_child;
        bool       entered;
        uint32_t  *saved;
    } RenameFrame;
    PIR_VEC(RenameFrame) stk = {0};
    RenameFrame init = { root_b, 0u, false, NULL };
    PIR_VEC_PUSH(&stk, init);

    while (stk.len > 0u) {
        RenameFrame *fr = &stk.data[stk.len - 1u];
        PirBlockId b = fr->b;
        if (!fr->entered) {
            fr->entered = true;
            fr->saved = (uint32_t *)malloc(ctx->nslots * sizeof(uint32_t));
            if (!fr->saved) abort();
            for (uint32_t s = 0u; s < ctx->nslots; ++s) {
                fr->saved[s] = ctx->stacks[s].len;
                PirValueId pv = ctx->phi_param[(size_t)s * ctx->nblks + b];
                if (pv != PIR_INVALID_ID) {
                    PIR_VEC_PUSH(&ctx->stacks[s], pv);
                }
            }

            PirInstIdVec new_insts = {NULL, 0u, 0u};
            uint32_t old_len = ctx->fn->blocks.data[b].insts.len;
            for (uint32_t j = 0u; j < old_len; ++j) {
                PirInstId iid = ctx->fn->blocks.data[b].insts.data[j];
                PirInst *ins = &ctx->fn->insts.data[iid];
                remap_ops(ctx, ins->operands, ins->operand_count);

                /* Drop promoted allocas */
                if (ins->op == PIR_OP_ALLOCA_PTR && ins->results[0] < ctx->orig_nvals) {
                    int32_t si = ctx->val_to_slot[ins->results[0]];
                    if (si >= 0) continue;
                }

                /* Promote load from slot */
                if (ins->op == PIR_OP_LOAD && ins->operands[0].kind == PIR_OPER_VAL &&
                    ins->operands[0].u.val < ctx->orig_nvals) {
                    int32_t si = ctx->val_to_slot[ins->operands[0].u.val];
                    if (si >= 0) {
                        PirValueId dst_res = ins->results[0];
                        PirValueId cur = stack_top_or_zero(ctx, (uint32_t)si);
                        ctx->val_remap[dst_res] = cur;
                        continue;
                    }
                }

                /* Promote store to slot */
                if (ins->op == PIR_OP_STORE && ins->operands[1].kind == PIR_OPER_VAL &&
                    ins->operands[1].u.val < ctx->orig_nvals) {
                    int32_t si = ctx->val_to_slot[ins->operands[1].u.val];
                    if (si >= 0) {
                        PirOperand val_op = ins->operands[0];
                        PirType sty = ctx->slots[si].slot_type;
                        uint32_t sline = ins->src_line;
                        uint32_t loc = ins->loc_idx;
                        PirValueId sv = PIR_INVALID_ID;
                        if (val_op.kind == PIR_OPER_VAL) {
                            sv = val_op.u.val;
                        } else {
                            sv = pir_fn_new_val(ctx->mod, ctx->fn, "m2r_c", sty, b, PIR_INVALID_ID, 0u);
                            PirOpcode cop = (val_op.kind == PIR_OPER_SYM) ? PIR_OP_ADDR : PIR_OP_CONST;
                            PirInst ci = pir_make_inst(cop, sty, sline, loc);
                            ci.operands = pir_alloc_ops(ctx->mod, 1u);
                            ci.operands[0] = val_op;
                            ci.operand_count = 1u;
                            ci.results[0] = sv;
                            pir_fn_append_inst(ctx->fn, &new_insts, ci);
                        }
                        PIR_VEC_PUSH(&ctx->stacks[si], sv);
                        continue;
                    }
                }

                PIR_VEC_PUSH(&new_insts, iid);
            }

            PIR_VEC_FREE(&ctx->fn->blocks.data[b].insts);
            ctx->fn->blocks.data[b].insts = new_insts;

            /* Update terminator args & append phi arguments for outgoing edges. */
            if (ctx->fn->blocks.data[b].insts.len > 0u) {
                PirInstId tid = ctx->fn->blocks.data[b].insts.data[ctx->fn->blocks.data[b].insts.len - 1u];
                PirInst *term = &ctx->fn->insts.data[tid];
                remap_ops(ctx, term->targets[0].args, term->targets[0].arg_count);
                remap_ops(ctx, term->targets[1].args, term->targets[1].arg_count);
                for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                    remap_ops(ctx, term->switch_cases[c].target.args, term->switch_cases[c].target.arg_count);
                }

                PirOpcode top = term->op;
                if (top == PIR_OP_JMP) {
                    append_phi_args_to_target(ctx, tid, 0, 0u);
                } else if (top == PIR_OP_BR) {
                    append_phi_args_to_target(ctx, tid, 0, 0u);
                    append_phi_args_to_target(ctx, tid, 0, 1u);
                } else if (top == PIR_OP_SWITCH) {
                    append_phi_args_to_target(ctx, tid, 0, 0u);
                    uint32_t nc = ctx->fn->insts.data[tid].switch_case_count;
                    for (uint32_t c = 0u; c < nc; ++c) {
                        append_phi_args_to_target(ctx, tid, 1, c);
                    }
                }
            }
        }
        if (fr->next_child < ctx->dom->children[b].len) {
            PirBlockId child = ctx->dom->children[b].data[fr->next_child++];
            RenameFrame cf = { child, 0u, false, NULL };
            PIR_VEC_PUSH(&stk, cf);
        } else {
            for (uint32_t s = 0u; s < ctx->nslots; ++s) {
                ctx->stacks[s].len = fr->saved[s];
            }
            free(fr->saved);
            stk.len--;
        }
    }
    PIR_VEC_FREE(&stk);
}

/* =========================================================================
 * Constant-Offset SROA (Scalar Replacement of Aggregates)
 * =========================================================================
 * Splits non-escaping fixed-size alloca.ptr objects accessed exclusively at
 * non-overlapping constant byte offsets (e.g., local structs, tuples, and
 * inlined static-link capture frames) into independent scalar allocas so
 * mem2reg immediately promotes every field into pure SSA registers.
 */

#define SROA_MAX_SLICES 16u
#define SROA_MAX_ALLOC_BYTES 256u

typedef struct {
    int64_t    offset;
    uint32_t   size;
    uint32_t   max_align;
    PirType    ty;
    PirValueId new_alloca_val;
} SroaSlice;

typedef struct {
    PirInstId  alloca_inst;
    PirValueId alloca_val;
    uint64_t   alloc_size;
    uint32_t   src_line;
    bool       ok;
    bool       has_nonzero_off;
    uint32_t   nslices;
    SroaSlice  slices[SROA_MAX_SLICES];
} SroaCand;

static int64_t sroa_imm_to_i64(const PirOperand *c) {
    uint64_t v = c->u.imm_int.is_negative ? (UINT64_C(0) - c->u.imm_int.lo)
                                          : c->u.imm_int.lo;
    if (c->type.kind == PIR_TYPE_SCALAR && c->type.u.scalar == PIR_SCALAR_I32) {
        return (int64_t)(int32_t)(uint32_t)v;
    }
    return (int64_t)v;
}

static bool sroa_const_i64(const PirFunction *fn, PirOperand op, int64_t *out) {
    if (op.kind == PIR_OPER_IMM_INT) {
        *out = sroa_imm_to_i64(&op);
        return true;
    }
    if (op.kind == PIR_OPER_VAL && op.u.val < fn->values.len) {
        PirInstId did = fn->values.data[op.u.val].def_inst;
        if (did != PIR_INVALID_ID && fn->insts.data[did].op == PIR_OP_CONST &&
            fn->insts.data[did].operand_count == 1u &&
            fn->insts.data[did].operands[0].kind == PIR_OPER_IMM_INT) {
            *out = sroa_imm_to_i64(&fn->insts.data[did].operands[0]);
            return true;
        }
    }
    return false;
}

static uint32_t sroa_type_bytes(PirType ty, uint8_t ptr_bytes) {
    if (!pir_type_is_ssa(ty)) return 0u;
    uint32_t bits = pir_type_bits(ty, ptr_bytes);
    return bits / 8u;
}

static void sroa_disqualify_ops(const PirOperand *ops, uint32_t n,
                                const int32_t *val_base, uint32_t nvals,
                                SroaCand *cands) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL && ops[i].u.val < nvals) {
            int32_t ci = val_base[ops[i].u.val];
            if (ci >= 0) cands[ci].ok = false;
        }
    }
}

static void sroa_record_access(SroaCand *cand, int64_t off, PirType ty, uint32_t flags,
                               uint32_t align, uint8_t ptr_bytes) {
    if (!cand->ok) return;
    if ((flags & PIR_FLAG_VOLATILE) != 0u) { cand->ok = false; return; }
    uint32_t sz = sroa_type_bytes(ty, ptr_bytes);
    if (sz == 0u || off < 0 || (uint64_t)off + (uint64_t)sz > cand->alloc_size) {
        cand->ok = false;
        return;
    }
    uint32_t eff_al = (align > sz) ? align : sz;
    if (off != 0) cand->has_nonzero_off = true;
    for (uint32_t k = 0u; k < cand->nslices; ++k) {
        SroaSlice *sl = &cand->slices[k];
        if (sl->offset == off && sl->size == sz) {
            if (!pir_type_eq(sl->ty, ty)) cand->ok = false;
            if (eff_al > sl->max_align) sl->max_align = eff_al;
            return;
        }
        if (!(off + (int64_t)sz <= sl->offset || sl->offset + (int64_t)sl->size <= off)) {
            cand->ok = false;
            return;
        }
    }
    if (cand->nslices >= SROA_MAX_SLICES) {
        cand->ok = false;
        return;
    }
    SroaSlice *ns = &cand->slices[cand->nslices++];
    ns->offset         = off;
    ns->size           = sz;
    ns->max_align      = eff_al;
    ns->ty             = ty;
    ns->new_alloca_val = PIR_INVALID_ID;
}

static void run_sroa(PirModule *mod, PirFunction *fn, uint8_t ptr_bytes) {
    uint32_t nvals = fn->values.len;
    if (nvals == 0u || fn->blocks.len == 0u) return;

    int32_t *val_base = (int32_t *)malloc(nvals * sizeof(int32_t));
    int64_t *val_off  = (int64_t *)calloc(nvals, sizeof(int64_t));
    if (!val_base || !val_off) abort();
    for (uint32_t v = 0u; v < nvals; ++v) val_base[v] = -1;

    PIR_VEC(SroaCand) cands = {NULL, 0u, 0u};
    const PirBlock *entry = &fn->blocks.data[0];
    for (uint32_t j = 0u; j < entry->insts.len; ++j) {
        PirInstId iid = entry->insts.data[j];
        const PirInst *ins = &fn->insts.data[iid];
        if (ins->op != PIR_OP_ALLOCA_PTR || ins->results[0] == PIR_INVALID_ID ||
            ins->results[0] >= nvals || ins->imm_u64 == 0u ||
            ins->imm_u64 > SROA_MAX_ALLOC_BYTES) {
            continue;
        }
        SroaCand sc;
        memset(&sc, 0, sizeof(sc));
        sc.alloca_inst = iid;
        sc.alloca_val  = ins->results[0];
        sc.alloc_size  = ins->imm_u64;
        sc.src_line    = ins->src_line;
        sc.ok          = true;
        val_base[sc.alloca_val] = (int32_t)cands.len;
        val_off[sc.alloca_val]  = 0;
        PIR_VEC_PUSH(&cands, sc);
    }

    if (cands.len == 0u) {
        free(val_base);
        free(val_off);
        return;
    }

    PirCfg scfg;
    pir_cfg_build(&scfg, fn);

    /* Propagate constant-offset ptr_add chains from candidate allocas in reachable blocks. */
    for (uint32_t pass = 0u; pass < 8u; ++pass) {
        bool changed = false;
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            if (!scfg.reachable[b]) continue;
            const PirBlock *blk = &fn->blocks.data[b];
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
                if (ins->op != PIR_OP_PTR_ADD || ins->operand_count != 2u ||
                    ins->results[0] == PIR_INVALID_ID || ins->results[0] >= nvals ||
                    val_base[ins->results[0]] >= 0) {
                    continue;
                }
                if (ins->operands[0].kind != PIR_OPER_VAL || ins->operands[0].u.val >= nvals) {
                    continue;
                }
                int32_t ci = val_base[ins->operands[0].u.val];
                if (ci < 0) continue;
                int64_t delta = 0;
                if (sroa_const_i64(fn, ins->operands[1], &delta)) {
                    int64_t bo = val_off[ins->operands[0].u.val];
                    if ((delta > 0 && bo > INT64_MAX - delta) ||
                        (delta < 0 && bo < INT64_MIN - delta)) {
                        cands.data[ci].ok = false;
                    } else {
                        val_base[ins->results[0]] = ci;
                        val_off[ins->results[0]]  = bo + delta;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) break;
    }

    /* Verify all uses of candidate allocas and derived pointers.
     * Any reference in an unreachable block disqualifies the candidate. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            sroa_disqualify_ops(ins->targets[0].args, ins->targets[0].arg_count,
                                val_base, nvals, cands.data);
            sroa_disqualify_ops(ins->targets[1].args, ins->targets[1].arg_count,
                                val_base, nvals, cands.data);
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                sroa_disqualify_ops(ins->switch_cases[c].target.args,
                                    ins->switch_cases[c].target.arg_count,
                                    val_base, nvals, cands.data);
            }

            if (!scfg.reachable[b]) {
                sroa_disqualify_ops(ins->operands, ins->operand_count,
                                    val_base, nvals, cands.data);
                continue;
            }

            if (ins->op == PIR_OP_PTR_ADD && ins->operand_count == 2u) {
                sroa_disqualify_ops(&ins->operands[1], 1u, val_base, nvals, cands.data);
                if (ins->operands[0].kind == PIR_OPER_VAL && ins->operands[0].u.val < nvals) {
                    int32_t ci = val_base[ins->operands[0].u.val];
                    if (ci >= 0 && (ins->results[0] >= nvals || val_base[ins->results[0]] != ci)) {
                        cands.data[ci].ok = false;
                    }
                }
            } else if (ins->op == PIR_OP_LOAD) {
                if (ins->operands[0].kind == PIR_OPER_VAL && ins->operands[0].u.val < nvals) {
                    int32_t ci = val_base[ins->operands[0].u.val];
                    if (ci >= 0) {
                        sroa_record_access(&cands.data[ci], val_off[ins->operands[0].u.val],
                                           ins->ty, ins->flags, ins->align, ptr_bytes);
                    }
                }
            } else if (ins->op == PIR_OP_STORE) {
                sroa_disqualify_ops(&ins->operands[0], 1u, val_base, nvals, cands.data);
                if (ins->operands[1].kind == PIR_OPER_VAL && ins->operands[1].u.val < nvals) {
                    int32_t ci = val_base[ins->operands[1].u.val];
                    if (ci >= 0) {
                        sroa_record_access(&cands.data[ci], val_off[ins->operands[1].u.val],
                                           ins->ty, ins->flags, ins->align, ptr_bytes);
                    }
                }
            } else {
                sroa_disqualify_ops(ins->operands, ins->operand_count,
                                    val_base, nvals, cands.data);
            }
        }
    }
    pir_cfg_destroy(&scfg);

    /* Emit replacement scalar allocas for each qualifying aggregate candidate. */
    PirInstIdVec new_allocas = {NULL, 0u, 0u};
    PirType ptr_ty = pir_type_scalar(PIR_SCALAR_PTR);
    bool any_split = false;

    for (uint32_t ci = 0u; ci < cands.len; ++ci) {
        SroaCand *sc = &cands.data[ci];
        if (!sc->ok || !sc->has_nonzero_off || sc->nslices == 0u) {
            sc->ok = false;
            continue;
        }
        any_split = true;
        for (uint32_t k = 0u; k < sc->nslices; ++k) {
            SroaSlice *sl = &sc->slices[k];
            PirValueId nv = pir_fn_new_val(mod, fn, "sroa", ptr_ty, 0u, PIR_INVALID_ID, 0u);
            PirInst al = pir_make_inst(PIR_OP_ALLOCA_PTR, ptr_ty, sc->src_line, 0u);
            al.imm_u64    = (uint64_t)sl->size;
            al.align      = sl->max_align;
            al.results[0] = nv;
            PirInstId aid = pir_fn_append_inst(fn, NULL, al);
            PIR_VEC_PUSH(&new_allocas, aid);
            sl->new_alloca_val = nv;
        }
    }

    if (any_split) {
        /* Rewrite loads/stores to point directly at slice allocas and drop dead base/ptr_adds. */
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirBlock *blk = &fn->blocks.data[b];
            uint32_t out_j = 0u;
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                PirInstId iid = blk->insts.data[j];
                PirInst *ins = &fn->insts.data[iid];
                if ((ins->op == PIR_OP_ALLOCA_PTR || ins->op == PIR_OP_PTR_ADD) &&
                    ins->results[0] < nvals && val_base[ins->results[0]] >= 0 &&
                    cands.data[val_base[ins->results[0]]].ok) {
                    continue;
                }
                if (ins->op == PIR_OP_LOAD && ins->operands[0].kind == PIR_OPER_VAL &&
                    ins->operands[0].u.val < nvals) {
                    int32_t ci = val_base[ins->operands[0].u.val];
                    if (ci >= 0 && cands.data[ci].ok) {
                        int64_t off = val_off[ins->operands[0].u.val];
                        const SroaCand *sc = &cands.data[ci];
                        for (uint32_t k = 0u; k < sc->nslices; ++k) {
                            if (sc->slices[k].offset == off) {
                                ins->operands[0] = pir_op_val(sc->slices[k].new_alloca_val, ptr_ty);
                                break;
                            }
                        }
                    }
                } else if (ins->op == PIR_OP_STORE && ins->operands[1].kind == PIR_OPER_VAL &&
                           ins->operands[1].u.val < nvals) {
                    int32_t ci = val_base[ins->operands[1].u.val];
                    if (ci >= 0 && cands.data[ci].ok) {
                        int64_t off = val_off[ins->operands[1].u.val];
                        const SroaCand *sc = &cands.data[ci];
                        for (uint32_t k = 0u; k < sc->nslices; ++k) {
                            if (sc->slices[k].offset == off) {
                                ins->operands[1] = pir_op_val(sc->slices[k].new_alloca_val, ptr_ty);
                                break;
                            }
                        }
                    }
                }
                blk->insts.data[out_j++] = iid;
            }
            blk->insts.len = out_j;
        }

        /* Prepend new scalar allocas to entry block. */
        pir_fn_prepend_entry_insts(fn, &new_allocas);
    }

    PIR_VEC_FREE(&new_allocas);
    PIR_VEC_FREE(&cands);
    free(val_off);
    free(val_base);
}

void pir_opt_mem2reg(PirModule *mod, PirFunction *fn, const PirTargetSpec *target) {
    if (fn->blocks.len == 0u) return;

    /* Functions calling setjmp / __pir_setjmp (lowered PIR invoke) have implicit
     * abnormal CFG edges from unwinding calls/resume back to the setjmp landing pad.
     * Promoting stack slots to SSA across those invisible edges would drop stores
     * executed before an exception unwinds (or clobber callee-saved regs on longjmp). */
    if (pir_fn_has_returns_twice(fn)) return;

    uint8_t ptr_bytes = target ? target->ptr_bytes : 8u;
    run_sroa(mod, fn, ptr_bytes);

    uint32_t nvals = fn->values.len;

    /* Pre-canonicalize zero-offset ptr_add (%p = ptr_add %base, 0) so frontends that
     * emit ptr_add %alloca, 0 do not falsely mark allocas as escaped before GVN runs.
     * Resolve chains of arbitrary depth with cycle detection so no kept use ever
     * references a deleted instruction. */
    PirCfg zcfg;
    pir_cfg_build(&zcfg, fn);
    PirValueId *zero_alias = (PirValueId *)malloc(nvals * sizeof(PirValueId));
    if (!zero_alias) abort();
    for (uint32_t v = 0u; v < nvals; ++v) zero_alias[v] = PIR_INVALID_ID;
    bool has_zero_pa = false;
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        if (!zcfg.reachable[b]) continue;
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_PTR_ADD && ins->operand_count == 2u &&
                (ins->flags & PIR_FLAG_INBOUNDS) == 0u &&
                ins->results[0] < nvals && ins->operands[0].kind == PIR_OPER_VAL &&
                ins->operands[1].kind == PIR_OPER_IMM_INT &&
                ins->operands[1].u.imm_int.lo == 0u && ins->operands[1].u.imm_int.hi == 0u) {
                PirValueId base = ins->operands[0].u.val;
                if (base != ins->results[0]) {
                    zero_alias[ins->results[0]] = base;
                    has_zero_pa = true;
                }
            }
        }
    }
    if (has_zero_pa) {
        uint8_t *state = (uint8_t *)calloc(nvals, sizeof(uint8_t));
        PirValueId *path = (PirValueId *)malloc(nvals * sizeof(PirValueId));
        if (!state || !path) abort();
        for (uint32_t v = 0u; v < nvals; ++v) {
            if (zero_alias[v] == PIR_INVALID_ID || state[v] == 2u) continue;
            uint32_t plen = 0u;
            PirValueId cur = v;
            bool cyclic = false;
            while (cur < nvals && zero_alias[cur] != PIR_INVALID_ID) {
                if (state[cur] == 2u) {
                    cur = zero_alias[cur];
                    break;
                }
                if (state[cur] == 1u) {
                    cyclic = true;
                    break;
                }
                state[cur] = 1u;
                path[plen++] = cur;
                cur = zero_alias[cur];
            }
            if (cyclic) {
                /* Break cycles in unreachable code by keeping all cyclic nodes intact. */
                for (uint32_t k = 0u; k < plen; ++k) {
                    zero_alias[path[k]] = PIR_INVALID_ID;
                    state[path[k]] = 2u;
                }
            } else {
                for (uint32_t k = 0u; k < plen; ++k) {
                    zero_alias[path[k]] = cur;
                    state[path[k]] = 2u;
                }
            }
        }
        free(path);
        free(state);
        for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
            PirBlock *blk = &fn->blocks.data[b];
            uint32_t out_j = 0u;
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                PirInstId iid = blk->insts.data[j];
                PirInst *ins = &fn->insts.data[iid];
                if (zcfg.reachable[b] && ins->results[0] < nvals &&
                    zero_alias[ins->results[0]] != PIR_INVALID_ID) {
                    continue;
                }
                for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                    if (ins->operands[k].kind == PIR_OPER_VAL && ins->operands[k].u.val < nvals) {
                        PirValueId r = zero_alias[ins->operands[k].u.val];
                        if (r != PIR_INVALID_ID) ins->operands[k].u.val = r;
                    }
                }
                for (uint32_t t = 0u; t < 2u; ++t) {
                    for (uint32_t k = 0u; k < ins->targets[t].arg_count; ++k) {
                        if (ins->targets[t].args[k].kind == PIR_OPER_VAL &&
                            ins->targets[t].args[k].u.val < nvals) {
                            PirValueId r = zero_alias[ins->targets[t].args[k].u.val];
                            if (r != PIR_INVALID_ID) ins->targets[t].args[k].u.val = r;
                        }
                    }
                }
                for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                    for (uint32_t k = 0u; k < ins->switch_cases[c].target.arg_count; ++k) {
                        if (ins->switch_cases[c].target.args[k].kind == PIR_OPER_VAL &&
                            ins->switch_cases[c].target.args[k].u.val < nvals) {
                            PirValueId r = zero_alias[ins->switch_cases[c].target.args[k].u.val];
                            if (r != PIR_INVALID_ID) ins->switch_cases[c].target.args[k].u.val = r;
                        }
                    }
                }
                blk->insts.data[out_j++] = iid;
            }
            blk->insts.len = out_j;
        }
    }
    free(zero_alias);
    pir_cfg_destroy(&zcfg);

    int32_t *val_to_slot = (int32_t *)malloc(nvals * sizeof(int32_t));
    if (!val_to_slot) abort();
    for (uint32_t v = 0u; v < nvals; ++v) val_to_slot[v] = -1;

    PIR_VEC(Mem2RegSlot) cands = {NULL, 0u, 0u};
    const PirBlock *entry = &fn->blocks.data[0];
    for (uint32_t j = 0u; j < entry->insts.len; ++j) {
        PirInstId iid = entry->insts.data[j];
        const PirInst *ins = &fn->insts.data[iid];
        if (ins->op != PIR_OP_ALLOCA_PTR || ins->results[0] == PIR_INVALID_ID) continue;
        Mem2RegSlot s;
        memset(&s, 0, sizeof(s));
        s.alloca_inst = iid;
        s.alloca_val  = ins->results[0];
        s.alloc_size  = ins->imm_u64;
        s.promotable  = true;
        s.zero_val    = PIR_INVALID_ID;
        val_to_slot[s.alloca_val] = (int32_t)cands.len;
        PIR_VEC_PUSH(&cands, s);
    }

    if (cands.len == 0u) {
        free(val_to_slot);
        return;
    }

    PirCfg cfg;
    pir_cfg_build(&cfg, fn);

    /* Scan all uses across the function to check escape / volatile / type uniformity.
     * Any reference inside an unreachable block disqualifies the alloca so rename_block
     * (which only walks the reachable dom tree) never leaves a dangling use behind. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            disqualify_in_ops(ins->targets[0].args, ins->targets[0].arg_count,
                              val_to_slot, nvals, cands.data);
            disqualify_in_ops(ins->targets[1].args, ins->targets[1].arg_count,
                              val_to_slot, nvals, cands.data);
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                disqualify_in_ops(ins->switch_cases[c].target.args,
                                  ins->switch_cases[c].target.arg_count,
                                  val_to_slot, nvals, cands.data);
            }

            if (!cfg.reachable[b]) {
                disqualify_in_ops(ins->operands, ins->operand_count,
                                  val_to_slot, nvals, cands.data);
                continue;
            }

            if (ins->op == PIR_OP_LOAD) {
                if (ins->operands[0].kind == PIR_OPER_VAL && ins->operands[0].u.val < nvals) {
                    int32_t si = val_to_slot[ins->operands[0].u.val];
                    if (si >= 0) {
                        Mem2RegSlot *s = &cands.data[si];
                        uint32_t need = min_type_bytes(ins->ty, ptr_bytes);
                        if ((ins->flags & PIR_FLAG_VOLATILE) || !pir_type_is_ssa(ins->ty) ||
                            need == 0u || s->alloc_size < (uint64_t)need) {
                            s->promotable = false;
                        } else if (!s->has_type) {
                            s->slot_type = ins->ty;
                            s->has_type = true;
                        } else if (!pir_type_eq(s->slot_type, ins->ty)) {
                            s->promotable = false;
                        }
                    }
                }
            } else if (ins->op == PIR_OP_STORE) {
                /* Stored value operand: if it is a candidate alloca, the address escaped! */
                disqualify_in_ops(&ins->operands[0], 1u, val_to_slot, nvals, cands.data);
                if (ins->operands[1].kind == PIR_OPER_VAL && ins->operands[1].u.val < nvals) {
                    int32_t si = val_to_slot[ins->operands[1].u.val];
                    if (si >= 0) {
                        Mem2RegSlot *s = &cands.data[si];
                        uint32_t need = min_type_bytes(ins->ty, ptr_bytes);
                        if ((ins->flags & PIR_FLAG_VOLATILE) || !pir_type_is_ssa(ins->ty) ||
                            need == 0u || s->alloc_size < (uint64_t)need) {
                            s->promotable = false;
                        } else if (!s->has_type) {
                            s->slot_type = ins->ty;
                            s->has_type = true;
                        } else if (!pir_type_eq(s->slot_type, ins->ty)) {
                            s->promotable = false;
                        }
                    }
                }
            } else {
                disqualify_in_ops(ins->operands, ins->operand_count,
                                  val_to_slot, nvals, cands.data);
            }
        }
    }

    /* Compact promotable slots. */
    uint32_t nslots = 0u;
    for (uint32_t v = 0u; v < nvals; ++v) val_to_slot[v] = -1;
    for (uint32_t i = 0u; i < cands.len; ++i) {
        if (cands.data[i].promotable && cands.data[i].has_type) {
            cands.data[nslots] = cands.data[i];
            val_to_slot[cands.data[nslots].alloca_val] = (int32_t)nslots;
            nslots++;
        }
    }
    if (nslots == 0u) {
        pir_cfg_destroy(&cfg);
        PIR_VEC_FREE(&cands);
        free(val_to_slot);
        return;
    }

    PirDom dom;
    pir_dom_build(&dom, &cfg);
    uint32_t nblks = fn->blocks.len;
    PirBlockIdVec *df = (PirBlockIdVec *)calloc(nblks, sizeof(PirBlockIdVec));
    if (!df) abort();
    pir_dom_compute_frontiers(&cfg, &dom, df);

    PirValueId *phi_param = (PirValueId *)malloc((size_t)nslots * nblks * sizeof(PirValueId));
    bool *is_def  = (bool *)malloc(nblks * sizeof(bool));
    bool *in_wl   = (bool *)malloc(nblks * sizeof(bool));
    PirBlockId *wl = (PirBlockId *)malloc(nblks * sizeof(PirBlockId));
    if (!phi_param || !is_def || !in_wl || !wl) abort();
    for (size_t i = 0u; i < (size_t)nslots * nblks; ++i) phi_param[i] = PIR_INVALID_ID;

    /* Insert block parameters along the iterated dominance frontier for each slot. */
    for (uint32_t s = 0u; s < nslots; ++s) {
        memset(is_def, 0, nblks * sizeof(bool));
        memset(in_wl, 0, nblks * sizeof(bool));
        uint32_t wl_len = 0u;

        for (uint32_t b = 0u; b < nblks; ++b) {
            if (!cfg.reachable[b]) continue;
            const PirBlock *blk = &fn->blocks.data[b];
            for (uint32_t j = 0u; j < blk->insts.len; ++j) {
                const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
                if (ins->op == PIR_OP_STORE && ins->operands[1].kind == PIR_OPER_VAL &&
                    ins->operands[1].u.val == cands.data[s].alloca_val) {
                    if (!is_def[b]) {
                        is_def[b] = true;
                        in_wl[b] = true;
                        wl[wl_len++] = b;
                    }
                    break;
                }
            }
        }

        while (wl_len > 0u) {
            PirBlockId x = wl[--wl_len];
            for (uint32_t i = 0u; i < df[x].len; ++i) {
                PirBlockId y = df[x].data[i];
                if (y == 0u || phi_param[(size_t)s * nblks + y] != PIR_INVALID_ID) continue;
                uint32_t pidx = fn->blocks.data[y].params.len;
                PirValueId pv = pir_fn_new_val(mod, fn, "m2r", cands.data[s].slot_type,
                                               y, PIR_INVALID_ID, pidx);
                PIR_VEC_PUSH(&fn->blocks.data[y].params, pv);
                phi_param[(size_t)s * nblks + y] = pv;
                if (!in_wl[y]) {
                    in_wl[y] = true;
                    wl[wl_len++] = y;
                }
            }
        }
    }

    PirValueId *val_remap = (PirValueId *)malloc(nvals * sizeof(PirValueId));
    PirValueIdVec *stacks = (PirValueIdVec *)calloc(nslots, sizeof(PirValueIdVec));
    if (!val_remap || !stacks) abort();
    for (uint32_t v = 0u; v < nvals; ++v) val_remap[v] = PIR_INVALID_ID;

    RenameCtx rctx;
    rctx.mod         = mod;
    rctx.fn          = fn;
    rctx.dom         = &dom;
    rctx.slots       = cands.data;
    rctx.nslots      = nslots;
    rctx.nblks       = nblks;
    rctx.orig_nvals  = nvals;
    rctx.val_to_slot = val_to_slot;
    rctx.phi_param   = phi_param;
    rctx.val_remap   = val_remap;
    rctx.stacks      = stacks;
    rctx.entry_zeros.data = NULL;
    rctx.entry_zeros.len  = 0u;
    rctx.entry_zeros.cap  = 0u;

    rename_block(&rctx, 0u);

    /* Any unreachable block branching to a reachable merge block with newly inserted
     * phi parameters must also receive matching zero arguments so IR stays valid. */
    for (uint32_t b = 0u; b < nblks; ++b) {
        if (cfg.reachable[b]) continue;
        const PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) continue;
        PirInstId tid = blk->insts.data[blk->insts.len - 1u];
        PirOpcode top = fn->insts.data[tid].op;
        if (top == PIR_OP_JMP) {
            append_phi_args_to_target(&rctx, tid, 0, 0u);
        } else if (top == PIR_OP_BR) {
            append_phi_args_to_target(&rctx, tid, 0, 0u);
            append_phi_args_to_target(&rctx, tid, 0, 1u);
        } else if (top == PIR_OP_SWITCH) {
            append_phi_args_to_target(&rctx, tid, 0, 0u);
            uint32_t ncases = fn->insts.data[tid].switch_case_count;
            for (uint32_t c = 0u; c < ncases; ++c) {
                append_phi_args_to_target(&rctx, tid, 1, c);
            }
        }
    }

    /* Prepend any synthesized zero constants to the entry block now that renaming is done. */
    pir_fn_prepend_entry_insts(fn, &rctx.entry_zeros);

    /* Final sweep to ensure all backedge / cross-block references resolve through val_remap. */
    for (uint32_t b = 0u; b < nblks; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            remap_ops(&rctx, ins->operands, ins->operand_count);
            remap_ops(&rctx, ins->targets[0].args, ins->targets[0].arg_count);
            remap_ops(&rctx, ins->targets[1].args, ins->targets[1].arg_count);
            for (uint32_t c = 0u; c < ins->switch_case_count; ++c) {
                remap_ops(&rctx, ins->switch_cases[c].target.args,
                          ins->switch_cases[c].target.arg_count);
            }
        }
    }

    for (uint32_t s = 0u; s < nslots; ++s) PIR_VEC_FREE(&stacks[s]);
    free(stacks);
    free(val_remap);
    free(wl);
    free(in_wl);
    free(is_def);
    free(phi_param);
    for (uint32_t b = 0u; b < nblks; ++b) PIR_VEC_FREE(&df[b]);
    free(df);
    pir_dom_destroy(&dom);
    pir_cfg_destroy(&cfg);
    PIR_VEC_FREE(&cands);
    free(val_to_slot);
}
