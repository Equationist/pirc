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

static bool eval_const_int(const PirFunction *fn, PirOperand op, uint64_t *out) {
    if (op.kind == PIR_OPER_VAL) {
        if (op.u.val >= fn->values.len) return false;
        PirInstId did = fn->values.data[op.u.val].def_inst;
        if (did == PIR_INVALID_ID || fn->insts.data[did].op != PIR_OP_CONST) return false;
        op = fn->insts.data[did].operands[0];
    }
    if (op.kind != PIR_OPER_IMM_INT) return false;
    *out = op.u.imm_int.is_negative ? (0u - op.u.imm_int.lo) : op.u.imm_int.lo;
    return true;
}

static bool target_eq(const PirBranchTarget *a, const PirBranchTarget *b) {
    if (a->target != b->target || a->arg_count != b->arg_count) return false;
    for (uint32_t i = 0u; i < a->arg_count; ++i) {
        if (!pir_operand_eq_exact(a->args[i], b->args[i])) return false;
    }
    return true;
}

static uint64_t int_width_mask(PirType ty) {
    if (ty.kind != PIR_TYPE_SCALAR) return UINT64_MAX;
    uint32_t bits = pir_scalar_bits(ty.u.scalar, 8u);
    if (bits == 0u || bits >= 64u) return UINT64_MAX;
    return (1ULL << bits) - 1ULL;
}

static bool fold_const_terminators(PirFunction *fn) {
    bool changed = false;
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) continue;
        PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
        if (term->op == PIR_OP_BR) {
            uint64_t cv = 0u;
            if (eval_const_int(fn, term->operands[0], &cv)) {
                uint32_t taken = ((cv & 0xffu) != 0u) ? 0u : 1u;
                PirBranchTarget chosen = term->targets[taken];
                term->op = PIR_OP_JMP;
                term->operand_count = 0u;
                term->operands = NULL;
                term->targets[0] = chosen;
                memset(&term->targets[1], 0, sizeof(term->targets[1]));
                term->targets[1].target = PIR_INVALID_ID;
                changed = true;
            } else if (target_eq(&term->targets[0], &term->targets[1])) {
                term->op = PIR_OP_JMP;
                term->operand_count = 0u;
                term->operands = NULL;
                memset(&term->targets[1], 0, sizeof(term->targets[1]));
                term->targets[1].target = PIR_INVALID_ID;
                changed = true;
            }
        } else if (term->op == PIR_OP_SWITCH) {
            uint64_t cv = 0u;
            if (eval_const_int(fn, term->operands[0], &cv)) {
                uint64_t mask = int_width_mask(term->ty);
                uint64_t mv   = cv & mask;
                PirBranchTarget chosen = term->targets[0];
                for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                    uint64_t kv = term->switch_cases[c].is_negative
                                  ? (0u - term->switch_cases[c].case_lo)
                                  : term->switch_cases[c].case_lo;
                    if ((kv & mask) == mv) {
                        chosen = term->switch_cases[c].target;
                        break;
                    }
                }
                term->op = PIR_OP_JMP;
                term->ty = pir_type_scalar(PIR_SCALAR_VOID);
                term->operand_count = 0u;
                term->operands = NULL;
                term->targets[0] = chosen;
                memset(&term->targets[1], 0, sizeof(term->targets[1]));
                term->targets[1].target = PIR_INVALID_ID;
                term->switch_cases = NULL;
                term->switch_case_count = 0u;
                changed = true;
            }
        }
    }
    return changed;
}

static bool prune_unreachable_blocks(PirFunction *fn, const PirCfg *cfg) {
    uint32_t n = fn->blocks.len;
    bool any_dead = false;
    for (uint32_t b = 0u; b < n; ++b) {
        if (!cfg->reachable[b]) {
            any_dead = true;
            break;
        }
    }
    if (!any_dead) return false;

    PirBlockId *remap = (PirBlockId *)malloc(n * sizeof(PirBlockId));
    if (!remap) abort();
    uint32_t dst = 0u;
    for (uint32_t b = 0u; b < n; ++b) {
        if (cfg->reachable[b]) {
            remap[b] = dst;
            if (dst != b) {
                fn->blocks.data[dst] = fn->blocks.data[b];
            }
            dst++;
        } else {
            remap[b] = PIR_INVALID_ID;
            PIR_VEC_FREE(&fn->blocks.data[b].params);
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
        }
    }
    fn->blocks.len = dst;

    for (uint32_t v = 0u; v < fn->values.len; ++v) {
        PirBlockId db = fn->values.data[v].def_block;
        if (db != PIR_INVALID_ID && db < n) {
            fn->values.data[v].def_block = remap[db];
        }
    }
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) continue;
        PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
        if (term->op == PIR_OP_JMP) {
            if (term->targets[0].target < n) term->targets[0].target = remap[term->targets[0].target];
        } else if (term->op == PIR_OP_BR || term->op == PIR_OP_INVOKE ||
                   term->op == PIR_OP_INVOKE_INDIRECT) {
            if (term->targets[0].target < n) term->targets[0].target = remap[term->targets[0].target];
            if (term->targets[1].target < n) term->targets[1].target = remap[term->targets[1].target];
        } else if (term->op == PIR_OP_SWITCH) {
            if (term->targets[0].target < n) term->targets[0].target = remap[term->targets[0].target];
            for (uint32_t c = 0u; c < term->switch_case_count; ++c) {
                PirBlockId t = term->switch_cases[c].target.target;
                if (t < n) {
                    term->switch_cases[c].target.target = remap[t];
                }
            }
        }
    }
    free(remap);
    return true;
}

static bool merge_single_pred(PirModule *mod, PirFunction *fn, const PirCfg *cfg) {
    for (uint32_t a = 0u; a < fn->blocks.len; ++a) {
        if (!cfg->reachable[a]) continue;
        PirBlock *ba = &fn->blocks.data[a];
        if (ba->insts.len == 0u) continue;
        PirInst *term = &fn->insts.data[ba->insts.data[ba->insts.len - 1u]];
        if (term->op != PIR_OP_JMP) continue;

        PirBlockId b = term->targets[0].target;
        if (b == 0u || b == a || b >= fn->blocks.len) continue;
        if (cfg->preds[b].len != 1u || cfg->preds[b].data[0] != a) continue;

        /* Pop jmp from A and save its arguments. */
        PirBranchTarget jmp_bt = term->targets[0];
        ba->insts.len--;

        /* Bind B's block params to A's jump arguments. */
        uint32_t nparams = fn->blocks.data[b].params.len;
        uint32_t src_line = fn->blocks.data[b].src_line;
        for (uint32_t k = 0u; k < nparams; ++k) {
            PirValueId pv = fn->blocks.data[b].params.data[k];
            PirOperand arg = jmp_bt.args[k];
            if (arg.kind == PIR_OPER_VAL) {
                pir_fn_replace_val_uses(fn, pv, arg.u.val);
            } else {
                PirType pty = fn->values.data[pv].type;
                PirOpcode cop = (arg.kind == PIR_OPER_SYM) ? PIR_OP_ADDR : PIR_OP_CONST;
                PirInst ci = pir_make_inst(cop, pty, src_line, 0u);
                ci.operands = pir_alloc_ops(mod, 1u);
                ci.operands[0] = arg;
                ci.operand_count = 1u;
                ci.results[0] = pv;
                fn->values.data[pv].def_block = a;
                fn->values.data[pv].param_index = 0u;
                pir_fn_append_inst(fn, &fn->blocks.data[a].insts, ci);
            }
        }

        /* Move B's instructions into A. */
        uint32_t nb = fn->blocks.data[b].insts.len;
        for (uint32_t j = 0u; j < nb; ++j) {
            PirInstId iid = fn->blocks.data[b].insts.data[j];
            PIR_VEC_PUSH(&fn->blocks.data[a].insts, iid);
            const PirInst *ins = &fn->insts.data[iid];
            if (ins->results[0] != PIR_INVALID_ID) {
                fn->values.data[ins->results[0]].def_block = a;
            }
            if (ins->results[1] != PIR_INVALID_ID) {
                fn->values.data[ins->results[1]].def_block = a;
            }
        }
        fn->blocks.data[b].params.len = 0u;
        fn->blocks.data[b].insts.len = 0u;
        return true;
    }
    return false;
}

static bool args_dominate_all(const PirFunction *fn, const PirBranchTarget *bt) {
    for (uint32_t i = 0u; i < bt->arg_count; ++i) {
        if (bt->args[i].kind == PIR_OPER_VAL) {
            PirValueId v = bt->args[i].u.val;
            if (v >= fn->values.len || fn->values.data[v].def_block != PIR_INVALID_ID) {
                return false;
            }
        }
    }
    return true;
}

static PirBranchTarget clone_branch_target(PirModule *mod, PirBranchTarget bt) {
    PirBranchTarget copy = bt;
    if (bt.arg_count > 0u && bt.args) {
        copy.args = pir_alloc_ops(mod, bt.arg_count);
        memcpy(copy.args, bt.args, bt.arg_count * sizeof(PirOperand));
    } else {
        copy.args = NULL;
    }
    return copy;
}

static bool thread_empty_jumps(PirModule *mod, PirFunction *fn, const PirCfg *cfg) {
    bool changed = false;
    for (uint32_t b = 1u; b < fn->blocks.len; ++b) {
        if (!cfg->reachable[b]) continue;
        PirBlock *bb = &fn->blocks.data[b];
        if (bb->params.len != 0u || bb->insts.len != 1u) continue;
        PirInst *jmp = &fn->insts.data[bb->insts.data[0]];
        if (jmp->op != PIR_OP_JMP) continue;
        PirBlockId c = jmp->targets[0].target;
        if (c == b || c == 0u) continue;
        if (!args_dominate_all(fn, &jmp->targets[0])) continue;

        for (uint32_t pi = 0u; pi < cfg->preds[b].len; ++pi) {
            PirBlockId p = cfg->preds[b].data[pi];
            if (p == b) continue;
            PirBlock *bp = &fn->blocks.data[p];
            if (bp->insts.len == 0u) continue;
            PirInst *pt = &fn->insts.data[bp->insts.data[bp->insts.len - 1u]];
            if (pt->op == PIR_OP_JMP) {
                if (pt->targets[0].target == b) {
                    pt->targets[0] = clone_branch_target(mod, jmp->targets[0]);
                    changed = true;
                }
            } else if (pt->op == PIR_OP_BR) {
                if (pt->targets[0].target == b) {
                    pt->targets[0] = clone_branch_target(mod, jmp->targets[0]);
                    changed = true;
                }
                if (pt->targets[1].target == b) {
                    pt->targets[1] = clone_branch_target(mod, jmp->targets[0]);
                    changed = true;
                }
            } else if (pt->op == PIR_OP_SWITCH) {
                if (pt->targets[0].target == b) {
                    pt->targets[0] = clone_branch_target(mod, jmp->targets[0]);
                    changed = true;
                }
                for (uint32_t sc = 0u; sc < pt->switch_case_count; ++sc) {
                    if (pt->switch_cases[sc].target.target == b) {
                        pt->switch_cases[sc].target = clone_branch_target(mod, jmp->targets[0]);
                        changed = true;
                    }
                }
            }
        }
    }
    return changed;
}

static bool is_pure_speculatable_alu(const PirModule *mod, const PirFunction *fn,
                                     const PirInst *ins, bool allow_cmp) {
    if (ins->results[0] == PIR_INVALID_ID || ins->results[1] != PIR_INVALID_ID) return false;
    if (ins->ty.kind != PIR_TYPE_SCALAR) return false;
    if ((pir_inst_effects(mod, fn, ins) & PIR_EFF_PURE_HOISTABLE) == 0u) return false;

    PirOpcode op = ins->op;
    if (op == PIR_OP_CONST) {
        /* Never speculatively hoist symbol addresses (e.g., weak extern TLS or GOT loads). */
        return ins->operand_count >= 1u && ins->operands[0].kind != PIR_OPER_SYM;
    }
    if (op == PIR_OP_SELECT ||
        op == PIR_OP_ADD || op == PIR_OP_SUB || op == PIR_OP_MUL ||
        op == PIR_OP_AND || op == PIR_OP_OR || op == PIR_OP_XOR ||
        op == PIR_OP_SHL || op == PIR_OP_SHR || op == PIR_OP_SAR ||
        op == PIR_OP_ZEXT || op == PIR_OP_SEXT || op == PIR_OP_TRUNC ||
        op == PIR_OP_FADD || op == PIR_OP_FSUB ||
        op == PIR_OP_FMUL || op == PIR_OP_FNEG || op == PIR_OP_SITOFP ||
        op == PIR_OP_UITOFP || op == PIR_OP_FPEXT || op == PIR_OP_FPTRUNC ||
        op == PIR_OP_BITCAST || op == PIR_OP_PTR_ADD) {
        return true;
    }
    if (allow_cmp && ((op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U) ||
                      (op >= PIR_OP_FCMP_OEQ && op <= PIR_OP_FCMP_UGE))) {
        return true;
    }
    return false;
}

/* If-convert trivial triangle/diamond branches into branchless select instructions:
 *   B: br %cond, .S, .M(f_args)     where .S has 0..3 pure ALU insts and jumps to .M(t_args)
 *   (or mirrored: br %cond, .M(t_args), .S where .S jumps to .M(f_args))
 * into:
 *   B: <hoisted pure ALU insts>; %sel_k = select %cond, t_arg_k, f_arg_k; jmp .M(...) */
static bool if_convert_diamonds(PirModule *mod, PirFunction *fn, const PirCfg *cfg) {
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        if (!cfg->reachable[b]) continue;
        PirBlock *bb = &fn->blocks.data[b];
        if (bb->insts.len == 0u) continue;
        PirInstId term_id = bb->insts.data[bb->insts.len - 1u];
        PirInst *term = &fn->insts.data[term_id];
        if (term->op != PIR_OP_BR) continue;

        PirBlockId t0 = term->targets[0].target;
        PirBlockId t1 = term->targets[1].target;
        if (t0 == t1 || t0 >= fn->blocks.len || t1 >= fn->blocks.len) continue;

        for (int side_idx = 0; side_idx < 2; ++side_idx) {
            PirBlockId sblk = (side_idx == 0) ? t0 : t1;
            PirBlockId mblk = (side_idx == 0) ? t1 : t0;
            if (sblk == 0u || sblk == b || mblk == 0u || mblk == b) continue;
            if (cfg->preds[sblk].len != 1u || cfg->preds[sblk].data[0] != b) continue;

            const PirBlock *sb = &fn->blocks.data[sblk];
            if (sb->params.len != 0u || sb->insts.len == 0u || sb->insts.len > 4u) continue;
            uint32_t n_alu = sb->insts.len - 1u;
            const PirInst *sjmp = &fn->insts.data[sb->insts.data[n_alu]];
            if (sjmp->op != PIR_OP_JMP || sjmp->targets[0].target != mblk) continue;

            bool alu_ok = true;
            for (uint32_t j = 0u; j < n_alu; ++j) {
                if (!is_pure_speculatable_alu(mod, fn, &fn->insts.data[sb->insts.data[j]], false)) {
                    alu_ok = false;
                    break;
                }
            }
            if (!alu_ok) continue;

            PirBranchTarget t_bt = (side_idx == 0) ? sjmp->targets[0] : term->targets[0];
            PirBranchTarget f_bt = (side_idx == 0) ? term->targets[1] : sjmp->targets[0];
            uint32_t nparams = fn->blocks.data[mblk].params.len;
            if (t_bt.arg_count != nparams || f_bt.arg_count != nparams) continue;
            if (nparams > 4u) continue;

            bool ok = true;
            for (uint32_t k = 0u; k < nparams; ++k) {
                PirType pty = fn->values.data[fn->blocks.data[mblk].params.data[k]].type;
                if (pty.kind != PIR_TYPE_SCALAR) { ok = false; break; }
            }
            if (!ok) continue;

            /* Pop br from B, hoist S's pure ALU insts into B, emit selects, then jmp .M. */
            PirOperand cond_op = term->operands[0];
            uint32_t sline = term->src_line;
            uint32_t sloc  = term->loc_idx;
            fn->blocks.data[b].insts.len--;

            for (uint32_t j = 0u; j < n_alu; ++j) {
                PirInstId siid = fn->blocks.data[sblk].insts.data[j];
                PirInst *sins = &fn->insts.data[siid];
                if ((sins->op == PIR_OP_ADD || sins->op == PIR_OP_SUB || sins->op == PIR_OP_MUL) &&
                    (sins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
                    sins->flags &= ~(uint32_t)(PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U);
                    sins->flags |= PIR_FLAG_WRAP;
                }
                if (sins->op == PIR_OP_PTR_ADD) {
                    sins->flags &= ~(uint32_t)PIR_FLAG_INBOUNDS;
                }
                sins->flags &= ~(uint32_t)PIR_FLAG_FAST;
                PIR_VEC_PUSH(&fn->blocks.data[b].insts, siid);
                PirValueId rv = sins->results[0];
                if (rv != PIR_INVALID_ID) {
                    fn->values.data[rv].def_block = b;
                }
            }
            fn->blocks.data[sblk].insts.len = 0u;

            PirOperand *margs = nparams ? pir_alloc_ops(mod, nparams) : NULL;
            for (uint32_t k = 0u; k < nparams; ++k) {
                if (pir_operand_eq_exact(t_bt.args[k], f_bt.args[k])) {
                    margs[k] = t_bt.args[k];
                } else {
                    PirType pty = fn->values.data[fn->blocks.data[mblk].params.data[k]].type;
                    PirValueId sv = pir_fn_new_val(mod, fn, "ifc", pty, b, PIR_INVALID_ID, 0u);
                    PirInst sel = pir_make_inst(PIR_OP_SELECT, pty, sline, sloc);
                    sel.operands = pir_alloc_ops(mod, 3u);
                    sel.operands[0] = cond_op;
                    sel.operands[1] = t_bt.args[k];
                    sel.operands[2] = f_bt.args[k];
                    sel.operand_count = 3u;
                    sel.results[0] = sv;
                    pir_fn_append_inst(fn, &fn->blocks.data[b].insts, sel);
                    margs[k] = pir_op_val(sv, pty);
                }
            }
            PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), sline, sloc);
            jmp.targets[0].target    = mblk;
            jmp.targets[0].args      = margs;
            jmp.targets[0].arg_count = nparams;
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jmp);
            return true;
        }

        /* Also handle symmetric 2-sided diamonds:
         *   B: br %cond, .T, .F  where both .T and .F have single pred B, 0..3 pure ALU insts,
         *      and both jump to the same merge block .M! */
        if (t0 != 0u && t0 != b && t1 != 0u && t1 != b &&
            cfg->preds[t0].len == 1u && cfg->preds[t0].data[0] == b &&
            cfg->preds[t1].len == 1u && cfg->preds[t1].data[0] == b) {
            const PirBlock *tb = &fn->blocks.data[t0];
            const PirBlock *fb = &fn->blocks.data[t1];
            if (tb->params.len == 0u && fb->params.len == 0u &&
                tb->insts.len >= 1u && tb->insts.len <= 4u &&
                fb->insts.len >= 1u && fb->insts.len <= 4u) {
                uint32_t nt = tb->insts.len - 1u;
                uint32_t nf = fb->insts.len - 1u;
                const PirInst *tjmp = &fn->insts.data[tb->insts.data[nt]];
                const PirInst *fjmp = &fn->insts.data[fb->insts.data[nf]];
                if (tjmp->op == PIR_OP_JMP && fjmp->op == PIR_OP_JMP &&
                    tjmp->targets[0].target == fjmp->targets[0].target &&
                    tjmp->targets[0].target != b && tjmp->targets[0].target != 0u) {
                    PirBlockId mblk = tjmp->targets[0].target;
                    uint32_t nparams = fn->blocks.data[mblk].params.len;
                    bool ok = (tjmp->targets[0].arg_count == nparams &&
                               fjmp->targets[0].arg_count == nparams &&
                               nparams <= 4u);
                    for (uint32_t j = 0u; j < nt && ok; ++j) {
                        if (!is_pure_speculatable_alu(mod, fn, &fn->insts.data[tb->insts.data[j]], false)) ok = false;
                    }
                    for (uint32_t j = 0u; j < nf && ok; ++j) {
                        if (!is_pure_speculatable_alu(mod, fn, &fn->insts.data[fb->insts.data[j]], false)) ok = false;
                    }
                    for (uint32_t k = 0u; k < nparams && ok; ++k) {
                        if (fn->values.data[fn->blocks.data[mblk].params.data[k]].type.kind != PIR_TYPE_SCALAR) {
                            ok = false;
                        }
                    }
                    if (ok) {
                        PirBranchTarget t_bt = tjmp->targets[0];
                        PirBranchTarget f_bt = fjmp->targets[0];
                        PirOperand cond_op   = term->operands[0];
                        uint32_t sline       = term->src_line;
                        uint32_t sloc        = term->loc_idx;
                        fn->blocks.data[b].insts.len--;

                        for (uint32_t j = 0u; j < nt; ++j) {
                            PirInstId siid = fn->blocks.data[t0].insts.data[j];
                            PirInst *sins = &fn->insts.data[siid];
                            if ((sins->op == PIR_OP_ADD || sins->op == PIR_OP_SUB || sins->op == PIR_OP_MUL) &&
                                (sins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
                                sins->flags &= ~(uint32_t)(PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U);
                                sins->flags |= PIR_FLAG_WRAP;
                            }
                            if (sins->op == PIR_OP_PTR_ADD) {
                                sins->flags &= ~(uint32_t)PIR_FLAG_INBOUNDS;
                            }
                            sins->flags &= ~(uint32_t)PIR_FLAG_FAST;
                            PIR_VEC_PUSH(&fn->blocks.data[b].insts, siid);
                            if (sins->results[0] != PIR_INVALID_ID) fn->values.data[sins->results[0]].def_block = b;
                        }
                        for (uint32_t j = 0u; j < nf; ++j) {
                            PirInstId siid = fn->blocks.data[t1].insts.data[j];
                            PirInst *sins = &fn->insts.data[siid];
                            if ((sins->op == PIR_OP_ADD || sins->op == PIR_OP_SUB || sins->op == PIR_OP_MUL) &&
                                (sins->flags & (PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U)) != 0u) {
                                sins->flags &= ~(uint32_t)(PIR_FLAG_UNSAFE_S | PIR_FLAG_UNSAFE_U);
                                sins->flags |= PIR_FLAG_WRAP;
                            }
                            if (sins->op == PIR_OP_PTR_ADD) {
                                sins->flags &= ~(uint32_t)PIR_FLAG_INBOUNDS;
                            }
                            sins->flags &= ~(uint32_t)PIR_FLAG_FAST;
                            PIR_VEC_PUSH(&fn->blocks.data[b].insts, siid);
                            if (sins->results[0] != PIR_INVALID_ID) fn->values.data[sins->results[0]].def_block = b;
                        }
                        fn->blocks.data[t0].insts.len = 0u;
                        fn->blocks.data[t1].insts.len = 0u;

                        PirOperand *margs = nparams ? pir_alloc_ops(mod, nparams) : NULL;
                        for (uint32_t k = 0u; k < nparams; ++k) {
                            if (pir_operand_eq_exact(t_bt.args[k], f_bt.args[k])) {
                                margs[k] = t_bt.args[k];
                            } else {
                                PirType pty = fn->values.data[fn->blocks.data[mblk].params.data[k]].type;
                                PirValueId sv = pir_fn_new_val(mod, fn, "ifc", pty, b, PIR_INVALID_ID, 0u);
                                PirInst sel = pir_make_inst(PIR_OP_SELECT, pty, sline, sloc);
                                sel.operands = pir_alloc_ops(mod, 3u);
                                sel.operands[0] = cond_op;
                                sel.operands[1] = t_bt.args[k];
                                sel.operands[2] = f_bt.args[k];
                                sel.operand_count = 3u;
                                sel.results[0] = sv;
                                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, sel);
                                margs[k] = pir_op_val(sv, pty);
                            }
                        }
                        PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), sline, sloc);
                        jmp.targets[0].target    = mblk;
                        jmp.targets[0].args      = margs;
                        jmp.targets[0].arg_count = nparams;
                        pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jmp);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static PirOperand subst_blk_param_op(PirOperand op, const PirBlock *blk, const PirBranchTarget *edge) {
    if (op.kind != PIR_OPER_VAL) return op;
    for (uint32_t k = 0u; k < blk->params.len && k < edge->arg_count; ++k) {
        if (blk->params.data[k] == op.u.val) return edge->args[k];
    }
    return op;
}

/* Tail-duplicate tiny 1-arithmetic-inst + jmp latch blocks (e.g. for_inc after switch/if)
 * into their jmp-only predecessors, eliminating an extra unconditional branch per loop iter. */
static bool tail_dup_tiny_jmp_blocks(PirModule *mod, PirFunction *fn, const PirCfg *cfg) {
    for (uint32_t b = 1u; b < fn->blocks.len; ++b) {
        if (!cfg->reachable[b]) continue;
        const PirBlock *bb = &fn->blocks.data[b];
        if (bb->insts.len != 2u || bb->params.len > 8u) continue;
        uint32_t npreds = cfg->preds[b].len;
        if (npreds < 2u || npreds > 16u) continue;

        const PirInst *i0   = &fn->insts.data[bb->insts.data[0]];
        const PirInst *bjmp = &fn->insts.data[bb->insts.data[1]];
        if (bjmp->op != PIR_OP_JMP || bjmp->targets[0].target == b) continue;
        if ((i0->op != PIR_OP_ADD && i0->op != PIR_OP_SUB) ||
            i0->ty.kind != PIR_TYPE_SCALAR ||
            (i0->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u ||
            i0->results[0] == PIR_INVALID_ID || i0->results[1] != PIR_INVALID_ID) {
            continue;
        }

        /* Ensure all predecessors end in unconditional JMP B (and none is B itself). */
        bool all_jmp = true;
        for (uint32_t pi = 0u; pi < npreds; ++pi) {
            PirBlockId p = cfg->preds[b].data[pi];
            if (p == b || p >= fn->blocks.len) { all_jmp = false; break; }
            const PirBlock *pb = &fn->blocks.data[p];
            if (pb->insts.len == 0u) { all_jmp = false; break; }
            const PirInst *pt = &fn->insts.data[pb->insts.data[pb->insts.len - 1u]];
            if (pt->op != PIR_OP_JMP || pt->targets[0].target != b ||
                pt->targets[0].arg_count != bb->params.len) {
                all_jmp = false; break;
            }
        }
        if (!all_jmp) continue;

        /* Ensure neither i0's result nor B's params are referenced outside B. */
        PirValueId i0_res = i0->results[0];
        bool escaped = false;
        for (uint32_t ob = 0u; ob < fn->blocks.len && !escaped; ++ob) {
            if (ob == b || !cfg->reachable[ob]) continue;
            const PirBlock *oblk = &fn->blocks.data[ob];
            for (uint32_t j = 0u; j < oblk->insts.len && !escaped; ++j) {
                const PirInst *oins = &fn->insts.data[oblk->insts.data[j]];
                for (uint32_t k = 0u; k < oins->operand_count; ++k) {
                    if (oins->operands[k].kind != PIR_OPER_VAL) continue;
                    PirValueId uv = oins->operands[k].u.val;
                    if (uv == i0_res) { escaped = true; break; }
                    for (uint32_t pk = 0u; pk < bb->params.len; ++pk) {
                        if (uv == bb->params.data[pk]) { escaped = true; break; }
                    }
                }
                for (uint32_t t = 0u; t < 2u && !escaped; ++t) {
                    for (uint32_t k = 0u; k < oins->targets[t].arg_count; ++k) {
                        if (oins->targets[t].args[k].kind != PIR_OPER_VAL) continue;
                        PirValueId uv = oins->targets[t].args[k].u.val;
                        if (uv == i0_res) { escaped = true; break; }
                        for (uint32_t pk = 0u; pk < bb->params.len; ++pk) {
                            if (uv == bb->params.data[pk]) { escaped = true; break; }
                        }
                    }
                }
                for (uint32_t sc = 0u; sc < oins->switch_case_count && !escaped; ++sc) {
                    for (uint32_t k = 0u; k < oins->switch_cases[sc].target.arg_count; ++k) {
                        if (oins->switch_cases[sc].target.args[k].kind != PIR_OPER_VAL) continue;
                        PirValueId uv = oins->switch_cases[sc].target.args[k].u.val;
                        if (uv == i0_res) { escaped = true; break; }
                        for (uint32_t pk = 0u; pk < bb->params.len; ++pk) {
                            if (uv == bb->params.data[pk]) { escaped = true; break; }
                        }
                    }
                }
            }
        }
        if (escaped) continue;

        /* Snapshot B's inst0, jmp target, and params before mutating fn->insts. */
        PirInst orig_i0   = *i0;
        PirBranchTarget dst_bt = bjmp->targets[0];
        uint32_t dst_argc = dst_bt.arg_count;
        uint32_t b_sline  = bjmp->src_line;
        uint32_t b_loc    = bjmp->loc_idx;

        for (uint32_t pi = 0u; pi < npreds; ++pi) {
            PirBlockId p = cfg->preds[b].data[pi];
            PirBlock *pb = &fn->blocks.data[p];
            PirInstId ptid = pb->insts.data[pb->insts.len - 1u];
            PirBranchTarget p_edge = fn->insts.data[ptid].targets[0];
            pb->insts.len--;

            PirValueId nv = pir_fn_new_val(mod, fn, "td", orig_i0.ty, p, PIR_INVALID_ID, 0u);
            PirInst ci = pir_make_inst(orig_i0.op, orig_i0.ty, orig_i0.src_line, orig_i0.loc_idx);
            ci.flags = orig_i0.flags;
            ci.operands = pir_alloc_ops(mod, orig_i0.operand_count);
            ci.operand_count = orig_i0.operand_count;
            for (uint32_t k = 0u; k < orig_i0.operand_count; ++k) {
                ci.operands[k] = subst_blk_param_op(orig_i0.operands[k], &fn->blocks.data[b], &p_edge);
            }
            ci.results[0] = nv;
            pir_fn_append_inst(fn, &fn->blocks.data[p].insts, ci);

            PirInst nj = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), b_sline, b_loc);
            nj.targets[0].target    = dst_bt.target;
            nj.targets[0].arg_count = dst_argc;
            if (dst_argc > 0u) {
                nj.targets[0].args = pir_alloc_ops(mod, dst_argc);
                for (uint32_t k = 0u; k < dst_argc; ++k) {
                    PirOperand a = dst_bt.args[k];
                    if (a.kind == PIR_OPER_VAL && a.u.val == i0_res) {
                        nj.targets[0].args[k] = pir_op_val(nv, orig_i0.ty);
                    } else {
                        nj.targets[0].args[k] = subst_blk_param_op(a, &fn->blocks.data[b], &p_edge);
                    }
                }
            }
            pir_fn_append_inst(fn, &fn->blocks.data[p].insts, nj);
        }
        return true;
    }
    return false;
}

static uint32_t count_val_uses_in_fn(const PirFunction *fn, PirValueId v) {
    uint32_t cnt = 0u;
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                if (ins->operands[k].kind == PIR_OPER_VAL && ins->operands[k].u.val == v) cnt++;
            }
            for (uint32_t t = 0u; t < 2u; ++t) {
                for (uint32_t k = 0u; k < ins->targets[t].arg_count; ++k) {
                    if (ins->targets[t].args[k].kind == PIR_OPER_VAL &&
                        ins->targets[t].args[k].u.val == v) cnt++;
                }
            }
            for (uint32_t sc = 0u; sc < ins->switch_case_count; ++sc) {
                for (uint32_t k = 0u; k < ins->switch_cases[sc].target.arg_count; ++k) {
                    if (ins->switch_cases[sc].target.args[k].kind == PIR_OPER_VAL &&
                        ins->switch_cases[sc].target.args[k].u.val == v) cnt++;
                }
            }
        }
    }
    return cnt;
}

static bool match_self_call_inst(const PirFunction *fn, const PirInst *ins) {
    return ins->op == PIR_OP_CALL &&
           ins->operand_count == 1u + fn->param_count &&
           ins->operands[0].kind == PIR_OPER_SYM &&
           ins->operands[0].u.sym.name != NULL &&
           strcmp(ins->operands[0].u.sym.name, fn->name) == 0 &&
           ins->results[1] == PIR_INVALID_ID;
}

static bool match_acc_tail_block(const PirFunction *fn, const PirBlock *blk,
                                 PirOpcode *out_op, PirOperand *out_other) {
    if (blk->insts.len < 3u) return false;
    if (fn->ret_type.kind != PIR_TYPE_SCALAR || !pir_scalar_is_int(fn->ret_type.u.scalar) ||
        fn->ret_type.u.scalar == PIR_SCALAR_I128) {
        return false;
    }
    const PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
    const PirInst *comb = &fn->insts.data[blk->insts.data[blk->insts.len - 2u]];
    const PirInst *call = &fn->insts.data[blk->insts.data[blk->insts.len - 3u]];
    if (term->op != PIR_OP_RET || term->operand_count != 1u ||
        term->operands[0].kind != PIR_OPER_VAL ||
        term->operands[0].u.val != comb->results[0]) {
        return false;
    }
    if ((comb->op != PIR_OP_ADD && comb->op != PIR_OP_MUL) ||
        !pir_type_eq(comb->ty, fn->ret_type) ||
        (comb->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u ||
        comb->operand_count != 2u || comb->results[1] != PIR_INVALID_ID) {
        return false;
    }
    if (!match_self_call_inst(fn, call) || call->results[0] == PIR_INVALID_ID) return false;
    PirValueId cr = call->results[0];
    bool op0_is_c = (comb->operands[0].kind == PIR_OPER_VAL && comb->operands[0].u.val == cr);
    bool op1_is_c = (comb->operands[1].kind == PIR_OPER_VAL && comb->operands[1].u.val == cr);
    if (op0_is_c == op1_is_c) return false; /* must be used on exactly one side */
    if (count_val_uses_in_fn(fn, cr) != 1u) return false;
    if (count_val_uses_in_fn(fn, comb->results[0]) != 1u) return false;
    if (out_op)    *out_op    = comb->op;
    if (out_other) *out_other = op0_is_c ? comb->operands[1] : comb->operands[0];
    return true;
}

static bool match_direct_tail_block(const PirFunction *fn, const PirBlock *blk) {
    if (blk->insts.len < 2u) return false;
    const PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
    const PirInst *call = &fn->insts.data[blk->insts.data[blk->insts.len - 2u]];
    if (!match_self_call_inst(fn, call)) return false;

    if (term->op == PIR_OP_RET) {
        bool void_ret = (term->operand_count == 0u && call->results[0] == PIR_INVALID_ID);
        bool val_ret  = (term->operand_count == 1u &&
                         term->operands[0].kind == PIR_OPER_VAL &&
                         term->operands[0].u.val == call->results[0]);
        return void_ret || val_ret;
    }
    if (term->op == PIR_OP_JMP) {
        PirBlockId dst = term->targets[0].target;
        if (dst >= fn->blocks.len) return false;
        const PirBlock *rblk = &fn->blocks.data[dst];
        if (rblk->insts.len != 1u) return false;
        const PirInst *rterm = &fn->insts.data[rblk->insts.data[0]];
        if (rterm->op != PIR_OP_RET) return false;
        if (call->results[0] == PIR_INVALID_ID) {
            return rblk->params.len == 0u && term->targets[0].arg_count == 0u &&
                   rterm->operand_count == 0u;
        }
        return rblk->params.len == 1u && term->targets[0].arg_count == 1u &&
               term->targets[0].args[0].kind == PIR_OPER_VAL &&
               term->targets[0].args[0].u.val == call->results[0] &&
               rterm->operand_count == 1u &&
               rterm->operands[0].kind == PIR_OPER_VAL &&
               rterm->operands[0].u.val == rblk->params.data[0] &&
               count_val_uses_in_fn(fn, call->results[0]) == 1u;
    }
    return false;
}

static bool eliminate_self_tail_calls(PirModule *mod, PirFunction *fn) {
    if (!fn->is_defined || fn->blocks.len == 0u || fn->is_variadic ||
        fn->linkage == PIR_LINK_WEAK ||
        fn->cabi_ret.kind != PIR_CABI_RET_NONE || !fn->name || fn->param_count > 6u) {
        return false;
    }
    for (uint32_t k = 0u; k < fn->param_count; ++k) {
        if (fn->params[k].abi_byval_size > 0u) return false;
    }
    /* Only safe when the function has no stack allocas / setjmp that could alias across iterations. */
    bool has_direct_tail = false;
    bool has_acc_tail    = false;
    PirOpcode acc_op     = PIR_OP_ADD;
    bool acc_op_set      = false;
    bool acc_conflict    = false;

    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_ALLOCA_PTR || ins->op == PIR_OP_ALLOCA_DYN ||
                ins->op == PIR_OP_STACKSAVE || ins->op == PIR_OP_STACKRESTORE ||
                ins->op == PIR_OP_VA_START) {
                return false;
            }
            if (ins->op == PIR_OP_CALL && ins->operand_count >= 1u &&
                ins->operands[0].kind == PIR_OPER_SYM &&
                pir_is_returns_twice_name(ins->operands[0].u.sym.name)) {
                return false;
            }
        }
        if (match_direct_tail_block(fn, blk)) {
            has_direct_tail = true;
        }
        PirOpcode bop = PIR_OP_ADD;
        if (match_acc_tail_block(fn, blk, &bop, NULL)) {
            if (!acc_op_set) { acc_op = bop; acc_op_set = true; }
            else if (acc_op != bop) { acc_conflict = true; }
            has_acc_tail = true;
        }
    }
    if (acc_conflict) has_acc_tail = false;
    if (!has_direct_tail && !has_acc_tail) return false;

    bool use_acc = has_acc_tail;
    uint32_t np = fn->param_count;
    uint32_t lp_argc = np + (use_acc ? 1u : 0u);
    PirBlockId body_bid = pir_fn_new_block(mod, fn, "tco_loop", fn->blocks.data[0].src_line);
    fn->blocks.data[body_bid].insts = fn->blocks.data[0].insts;

    PirValueId *new_params = np ? (PirValueId *)malloc(np * sizeof(PirValueId)) : NULL;
    if (np && !new_params) abort();

    for (uint32_t k = 0u; k < np; ++k) {
        PirType pty = fn->params[k].type;
        PirValueId bp = pir_fn_new_val(mod, fn, "tco_p", pty, body_bid, PIR_INVALID_ID, k);
        new_params[k] = bp;
        PIR_VEC_PUSH(&fn->blocks.data[body_bid].params, bp);
    }

    PirValueId acc_param = PIR_INVALID_ID;
    if (use_acc) {
        acc_param = pir_fn_new_val(mod, fn, "tco_acc", fn->ret_type, body_bid, PIR_INVALID_ID, np);
        PIR_VEC_PUSH(&fn->blocks.data[body_bid].params, acc_param);
    }

    /* Update def_block of instructions moved from blocks[0] to body_bid. */
    for (uint32_t j = 0u; j < fn->blocks.data[body_bid].insts.len; ++j) {
        PirInst *ins = &fn->insts.data[fn->blocks.data[body_bid].insts.data[j]];
        if (ins->results[0] != PIR_INVALID_ID) fn->values.data[ins->results[0]].def_block = body_bid;
        if (ins->results[1] != PIR_INVALID_ID) fn->values.data[ins->results[1]].def_block = body_bid;
    }

    /* Replace all uses of fn->params[k] across all existing blocks (1..body_bid) with new_params[k]. */
    fn->blocks.data[0].insts.data = NULL;
    fn->blocks.data[0].insts.len  = 0u;
    fn->blocks.data[0].insts.cap  = 0u;
    for (uint32_t k = 0u; k < np; ++k) {
        pir_fn_replace_val_uses(fn, fn->params[k].val_id, new_params[k]);
    }

    /* Populate new entry block[0]: jmp .tco_loop(%orig_p0, ..., [acc_init]) */
    PirInst ej = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), fn->src_line, 0u);
    ej.targets[0].target    = body_bid;
    ej.targets[0].arg_count = lp_argc;
    if (lp_argc > 0u) {
        ej.targets[0].args = pir_alloc_ops(mod, lp_argc);
        for (uint32_t k = 0u; k < np; ++k) {
            ej.targets[0].args[k] = pir_op_val(fn->params[k].val_id, fn->params[k].type);
        }
        if (use_acc) {
            uint64_t id_val = (acc_op == PIR_OP_MUL) ? 1u : 0u;
            ej.targets[0].args[np] = pir_op_imm_i(id_val, false, fn->ret_type);
        }
    }
    pir_fn_append_inst(fn, &fn->blocks.data[0].insts, ej);

    /* Pre-classify all blocks BEFORE mutating any block so rewriting an earlier block
     * (e.g., turning a 1-inst shared ret block into 2 insts) cannot perturb matching of
     * later blocks. */
    uint32_t nblks = fn->blocks.len;
    uint8_t *bkind = (uint8_t *)calloc(nblks, sizeof(uint8_t));
    PirOperand *acc_other = (PirOperand *)calloc(nblks, sizeof(PirOperand));
    if (!bkind || !acc_other) abort();

    for (uint32_t b = 1u; b < nblks; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        if (blk->insts.len == 0u) continue;
        PirOpcode bop = PIR_OP_ADD;
        PirOperand oop;
        memset(&oop, 0, sizeof(oop));
        if (use_acc && match_acc_tail_block(fn, blk, &bop, &oop) && bop == acc_op) {
            bkind[b] = 1u;
            acc_other[b] = oop;
        } else if (match_direct_tail_block(fn, blk)) {
            bkind[b] = 2u;
        } else if (use_acc) {
            const PirInst *term = &fn->insts.data[blk->insts.data[blk->insts.len - 1u]];
            if (term->op == PIR_OP_RET && term->operand_count == 1u) {
                bkind[b] = 3u;
            }
        }
    }

    /* Rewrite direct self-tail calls, accumulator self-tail calls, and base-case returns. */
    for (uint32_t b = 1u; b < nblks; ++b) {
        PirBlock *blk = &fn->blocks.data[b];
        if (bkind[b] == 1u) {
            /* Case 1: Accumulator self-tail call (`call; add/mul; ret`) */
            PirInstId cid = blk->insts.data[blk->insts.len - 3u];
            PirInst call_copy = fn->insts.data[cid];

            PirValueId nacc = pir_fn_new_val(mod, fn, "tco_a", fn->ret_type, b, PIR_INVALID_ID, 0u);
            PirInst ains = pir_make_inst(acc_op, fn->ret_type, call_copy.src_line, call_copy.loc_idx);
            ains.flags         = PIR_FLAG_WRAP;
            ains.operands      = pir_alloc_ops(mod, 2u);
            ains.operands[0]   = pir_op_val(acc_param, fn->ret_type);
            ains.operands[1]   = acc_other[b];
            ains.operand_count = 2u;
            ains.results[0]    = nacc;

            PirOperand *jargs = pir_alloc_ops(mod, lp_argc);
            for (uint32_t k = 0u; k < np; ++k) {
                PirOperand a = call_copy.operands[1u + k];
                pir_op_strip_call_meta(&a);
                jargs[k] = a;
            }
            jargs[np] = pir_op_val(nacc, fn->ret_type);

            PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID),
                                        call_copy.src_line, call_copy.loc_idx);
            jmp.targets[0].target    = body_bid;
            jmp.targets[0].arg_count = lp_argc;
            jmp.targets[0].args      = jargs;

            blk->insts.len -= 3u;
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, ains);
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jmp);
        } else if (bkind[b] == 2u) {
            /* Case 2: Direct self-tail call (`call; ret` or `call; jmp .ret_blk`) */
            PirInstId cid = blk->insts.data[blk->insts.len - 2u];
            PirInst call_copy = fn->insts.data[cid];
            PirOperand *jargs = lp_argc ? pir_alloc_ops(mod, lp_argc) : NULL;
            for (uint32_t k = 0u; k < np; ++k) {
                PirOperand a = call_copy.operands[1u + k];
                pir_op_strip_call_meta(&a);
                jargs[k] = a;
            }
            if (use_acc) jargs[np] = pir_op_val(acc_param, fn->ret_type);
            PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID),
                                        call_copy.src_line, call_copy.loc_idx);
            jmp.targets[0].target    = body_bid;
            jmp.targets[0].arg_count = lp_argc;
            jmp.targets[0].args      = jargs;
            blk->insts.len -= 2u;
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, jmp);
        } else if (bkind[b] == 3u) {
            /* Case 3: Base-case `ret %v` when using accumulator -> combine `%v` with `%tco_acc`! */
            PirInstId tid = blk->insts.data[blk->insts.len - 1u];
            PirInst term_copy = fn->insts.data[tid];
            PirValueId fin = pir_fn_new_val(mod, fn, "tco_r", fn->ret_type, b, PIR_INVALID_ID, 0u);
            PirInst comb = pir_make_inst(acc_op, fn->ret_type, term_copy.src_line, term_copy.loc_idx);
            comb.flags         = PIR_FLAG_WRAP;
            comb.operands      = pir_alloc_ops(mod, 2u);
            comb.operands[0]   = term_copy.operands[0];
            comb.operands[1]   = pir_op_val(acc_param, fn->ret_type);
            comb.operand_count = 2u;
            comb.results[0]    = fin;

            PirInst nret = pir_make_inst(PIR_OP_RET, fn->ret_type, term_copy.src_line, term_copy.loc_idx);
            nret.operands      = pir_alloc_ops(mod, 1u);
            nret.operands[0]   = pir_op_val(fin, fn->ret_type);
            nret.operand_count = 1u;

            blk->insts.len -= 1u;
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, comb);
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, nret);
        }
    }

    free(acc_other);
    free(bkind);
    free(new_params);
    return true;
}

void pir_opt_simplify_cfg(PirModule *mod, PirFunction *fn) {
    if (fn->blocks.len == 0u) return;
    {
        PirCfg init_cfg;
        pir_cfg_build(&init_cfg, fn);
        (void)prune_unreachable_blocks(fn, &init_cfg);
        pir_cfg_destroy(&init_cfg);
    }
    (void)eliminate_self_tail_calls(mod, fn);
    for (uint32_t iter = 0u; iter < 1024u; ++iter) {
        bool changed = false;
        if (fold_const_terminators(fn)) changed = true;

        PirCfg cfg;
        pir_cfg_build(&cfg, fn);
        if (prune_unreachable_blocks(fn, &cfg)) {
            pir_cfg_destroy(&cfg);
            continue;
        }
        if (if_convert_diamonds(mod, fn, &cfg)) {
            pir_cfg_destroy(&cfg);
            continue;
        }
        if (merge_single_pred(mod, fn, &cfg)) {
            pir_cfg_destroy(&cfg);
            continue;
        }
        if (tail_dup_tiny_jmp_blocks(mod, fn, &cfg)) {
            pir_cfg_destroy(&cfg);
            continue;
        }
        if (thread_empty_jumps(mod, fn, &cfg)) {
            changed = true;
        }
        pir_cfg_destroy(&cfg);
        if (!changed) break;
    }
    /* Always prune any blocks emptied on the final iteration (e.g., if the cap was hit). */
    {
        PirCfg final_cfg;
        pir_cfg_build(&final_cfg, fn);
        (void)prune_unreachable_blocks(fn, &final_cfg);
        pir_cfg_destroy(&final_cfg);
    }
}

static void split_one_edge(PirModule *mod, PirFunction *fn, PirInstId term_id,
                           int slot_kind, uint32_t idx, PirBlockId dest) {
    PirBlockId e = pir_fn_new_block(mod, fn, "edge", fn->insts.data[term_id].src_line);

    PirBranchTarget *bt = NULL;
    if (slot_kind == 0) {
        bt = &fn->insts.data[term_id].targets[idx];
    } else {
        bt = &fn->insts.data[term_id].switch_cases[idx].target;
    }

    PirInst jmp = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID),
                                fn->insts.data[term_id].src_line, 0u);
    jmp.targets[0].target    = dest;
    jmp.targets[0].args      = bt->args;
    jmp.targets[0].arg_count = bt->arg_count;

    bt->target    = e;
    bt->args      = NULL;
    bt->arg_count = 0u;

    pir_fn_append_inst(fn, &fn->blocks.data[e].insts, jmp);
}

void pir_opt_edge_split(PirModule *mod, PirFunction *fn) {
    if (fn->blocks.len == 0u) return;
    PirCfg cfg;
    pir_cfg_build(&cfg, fn);

    uint32_t orig_n = fn->blocks.len;
    for (uint32_t a = 0u; a < orig_n; ++a) {
        if (!cfg.reachable[a]) continue;
        PirBlock *ba = &fn->blocks.data[a];
        if (ba->insts.len == 0u) continue;
        PirInstId term_id = ba->insts.data[ba->insts.len - 1u];
        PirOpcode op = fn->insts.data[term_id].op;

        if (op == PIR_OP_BR) {
            for (uint32_t s = 0u; s < 2u; ++s) {
                PirBlockId dst = fn->insts.data[term_id].targets[s].target;
                if (dst < orig_n && cfg.preds[dst].len > 1u) {
                    split_one_edge(mod, fn, term_id, 0, s, dst);
                }
            }
        } else if (op == PIR_OP_SWITCH && fn->insts.data[term_id].switch_case_count > 0u) {
            PirBlockId def_dst = fn->insts.data[term_id].targets[0].target;
            if (def_dst < orig_n && cfg.preds[def_dst].len > 1u) {
                split_one_edge(mod, fn, term_id, 0, 0u, def_dst);
            }
            uint32_t nc = fn->insts.data[term_id].switch_case_count;
            for (uint32_t c = 0u; c < nc; ++c) {
                PirBlockId cdst = fn->insts.data[term_id].switch_cases[c].target.target;
                if (cdst < orig_n && cfg.preds[cdst].len > 1u) {
                    split_one_edge(mod, fn, term_id, 1, c, cdst);
                }
            }
        }
    }

    pir_cfg_destroy(&cfg);
}

/* =========================================================================
 * General Single-Body Loop Rotation (Top-Tested -> Bottom-Tested)
 * =========================================================================
 * Transforms:
 *   H(%h...): <0..3 pure cmp/alu>; br %c, .B, .E(exit_args)
 *   B:        <body>; jmp .H(next_args)
 * into:
 *   H(%h...): <0..3 pure cmp/alu>; br %c, .B(%h...), .E(exit_args)
 *   B(%b...): <body[h->b]>; <cloned cmp/alu[h->next]>; br %c', .B(next_args), .E(exit_args[h->next])
 * Eliminating 1 unconditional branch per inner-loop iteration!
 */

static bool val_used_in_ops(const PirOperand *ops, uint32_t n, PirValueId v) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL && ops[i].u.val == v) return true;
    }
    return false;
}

static bool val_used_in_inst(const PirInst *ins, PirValueId v) {
    if (val_used_in_ops(ins->operands, ins->operand_count, v)) return true;
    if (val_used_in_ops(ins->targets[0].args, ins->targets[0].arg_count, v)) return true;
    if (val_used_in_ops(ins->targets[1].args, ins->targets[1].arg_count, v)) return true;
    for (uint32_t sc = 0u; sc < ins->switch_case_count; ++sc) {
        if (val_used_in_ops(ins->switch_cases[sc].target.args,
                            ins->switch_cases[sc].target.arg_count, v)) {
            return true;
        }
    }
    return false;
}

static void replace_val_in_ops(PirOperand *ops, uint32_t n, PirValueId old_v, PirValueId new_v) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL && ops[i].u.val == old_v) {
            ops[i].u.val = new_v;
        }
    }
}

static void replace_val_in_block(PirFunction *fn, PirBlockId bid, PirValueId old_v, PirValueId new_v) {
    const PirBlock *blk = &fn->blocks.data[bid];
    for (uint32_t j = 0u; j < blk->insts.len; ++j) {
        PirInst *ins = &fn->insts.data[blk->insts.data[j]];
        replace_val_in_ops(ins->operands, ins->operand_count, old_v, new_v);
        replace_val_in_ops(ins->targets[0].args, ins->targets[0].arg_count, old_v, new_v);
        replace_val_in_ops(ins->targets[1].args, ins->targets[1].arg_count, old_v, new_v);
        for (uint32_t sc = 0u; sc < ins->switch_case_count; ++sc) {
            replace_val_in_ops(ins->switch_cases[sc].target.args,
                               ins->switch_cases[sc].target.arg_count, old_v, new_v);
        }
    }
}

static PirOperand map_rot_op(PirOperand op,
                             const PirValueId *h_params, const PirValueId *next_vals, uint32_t nparams,
                             const PirValueId *h_res, const PirValueId *c_res, uint32_t n_alu) {
    if (op.kind != PIR_OPER_VAL) return op;
    for (uint32_t k = 0u; k < nparams; ++k) {
        if (op.u.val == h_params[k]) {
            op.u.val = next_vals[k];
            return op;
        }
    }
    for (uint32_t m = 0u; m < n_alu; ++m) {
        if (op.u.val == h_res[m]) {
            op.u.val = c_res[m];
            return op;
        }
    }
    return op;
}

static bool try_rotate_one_loop(PirModule *mod, PirFunction *fn, const PirCfg *cfg) {
    for (uint32_t h = 1u; h < fn->blocks.len; ++h) {
        if (!cfg->reachable[h] || cfg->preds[h].len < 2u) continue;
        const PirBlock *hb = &fn->blocks.data[h];
        if (hb->insts.len == 0u || hb->insts.len > 4u || hb->params.len > 8u) continue;

        uint32_t n_alu = hb->insts.len - 1u;
        const PirInst *hterm = &fn->insts.data[hb->insts.data[n_alu]];
        if (hterm->op != PIR_OP_BR) continue;

        bool alu_ok = true;
        for (uint32_t j = 0u; j < n_alu; ++j) {
            if (!is_pure_speculatable_alu(mod, fn, &fn->insts.data[hb->insts.data[j]], true)) {
                alu_ok = false;
                break;
            }
        }
        if (!alu_ok) continue;

        for (int body_side = 0; body_side < 2; ++body_side) {
            int exit_side = 1 - body_side;
            PirBlockId b = hterm->targets[body_side].target;
            PirBlockId e = hterm->targets[exit_side].target;
            if (b == 0u || b == h || b == e || b >= fn->blocks.len) continue;
            if (e == 0u || e == h || e >= fn->blocks.len) continue;
            if (hterm->targets[body_side].arg_count != 0u) continue;
            if (cfg->preds[b].len != 1u || cfg->preds[b].data[0] != h) continue;

            const PirBlock *bb = &fn->blocks.data[b];
            if (bb->params.len != 0u || bb->insts.len == 0u) continue;
            const PirInst *bterm = &fn->insts.data[bb->insts.data[bb->insts.len - 1u]];
            if (bterm->op != PIR_OP_JMP || bterm->targets[0].target != h) continue;
            uint32_t nparams = hb->params.len;
            if (bterm->targets[0].arg_count != nparams) continue;

            /* Check 1: H's instruction results must not be used outside H or in exit_args.
             * Check 2: H's block params may be used outside {H, B} ONLY if E has H as its sole pred
             *          (in which case E dominates all external uses and receives them as new params). */
            bool escaped = false;
            bool param_ext_used[8] = { false, false, false, false, false, false, false, false };
            uint32_t n_ext_params = 0u;
            bool e_single_pred = (cfg->preds[e].len == 1u && cfg->preds[e].data[0] == h);
            PirValueId h_res[4];
            for (uint32_t j = 0u; j < n_alu; ++j) {
                h_res[j] = fn->insts.data[hb->insts.data[j]].results[0];
                if (val_used_in_ops(hterm->targets[exit_side].args,
                                    hterm->targets[exit_side].arg_count, h_res[j])) {
                    escaped = true;
                    break;
                }
            }
            for (uint32_t ob = 0u; ob < fn->blocks.len && !escaped; ++ob) {
                if (!cfg->reachable[ob] || ob == h) continue;
                const PirBlock *oblk = &fn->blocks.data[ob];
                for (uint32_t j = 0u; j < oblk->insts.len && !escaped; ++j) {
                    const PirInst *oins = &fn->insts.data[oblk->insts.data[j]];
                    for (uint32_t m = 0u; m < n_alu; ++m) {
                        if (val_used_in_inst(oins, h_res[m])) { escaped = true; break; }
                    }
                    if (!escaped && ob != b) {
                        for (uint32_t pk = 0u; pk < nparams; ++pk) {
                            if (val_used_in_inst(oins, hb->params.data[pk])) {
                                if (!e_single_pred) {
                                    escaped = true;
                                    break;
                                }
                                if (!param_ext_used[pk]) {
                                    param_ext_used[pk] = true;
                                    n_ext_params++;
                                }
                            }
                        }
                    }
                }
            }
            if (escaped) continue;

            /* Snapshot H metadata before mutating fn vectors. */
            PirValueId h_params[8];
            PirType    h_ptys[8];
            for (uint32_t k = 0u; k < nparams; ++k) {
                h_params[k] = hb->params.data[k];
                h_ptys[k]   = fn->values.data[h_params[k]].type;
            }
            PirInst h_alu_snap[4];
            for (uint32_t j = 0u; j < n_alu; ++j) {
                h_alu_snap[j] = fn->insts.data[hb->insts.data[j]];
            }
            PirInst hterm_snap = *hterm;
            PirInstId hterm_id = hb->insts.data[n_alu];

            /* 0. If any H params are used outside {H, B}, thread them through new params on E. */
            if (n_ext_params > 0u) {
                uint32_t old_e_argc = hterm_snap.targets[exit_side].arg_count;
                uint32_t new_e_argc = old_e_argc + n_ext_params;
                PirOperand *new_h_eargs = pir_alloc_ops(mod, new_e_argc);
                for (uint32_t k = 0u; k < old_e_argc; ++k) {
                    new_h_eargs[k] = hterm_snap.targets[exit_side].args[k];
                }
                uint32_t cur_slot = old_e_argc;
                for (uint32_t pk = 0u; pk < nparams; ++pk) {
                    if (!param_ext_used[pk]) continue;
                    PirValueId ep = pir_fn_new_val(mod, fn, "lre", h_ptys[pk], e, PIR_INVALID_ID,
                                                   fn->blocks.data[e].params.len);
                    PIR_VEC_PUSH(&fn->blocks.data[e].params, ep);
                    new_h_eargs[cur_slot++] = pir_op_val(h_params[pk], h_ptys[pk]);
                    for (uint32_t ob = 0u; ob < fn->blocks.len; ++ob) {
                        if (ob != h && ob != b) {
                            replace_val_in_block(fn, ob, h_params[pk], ep);
                        }
                    }
                }
                fn->insts.data[hterm_id].targets[exit_side].args      = new_h_eargs;
                fn->insts.data[hterm_id].targets[exit_side].arg_count = new_e_argc;
                hterm_snap.targets[exit_side].args      = new_h_eargs;
                hterm_snap.targets[exit_side].arg_count = new_e_argc;
            }

            /* 1. Give B fresh block params matching H's params and rewrite uses inside B. */
            for (uint32_t k = 0u; k < nparams; ++k) {
                PirValueId bp = pir_fn_new_val(mod, fn, "lr", h_ptys[k], b, PIR_INVALID_ID, k);
                PIR_VEC_PUSH(&fn->blocks.data[b].params, bp);
                replace_val_in_block(fn, b, h_params[k], bp);
            }

            /* 2. Update H's branch edge to B to pass H's params. */
            if (nparams > 0u) {
                PirOperand *h_to_b = pir_alloc_ops(mod, nparams);
                for (uint32_t k = 0u; k < nparams; ++k) {
                    h_to_b[k] = pir_op_val(h_params[k], h_ptys[k]);
                }
                fn->insts.data[hterm_id].targets[body_side].args      = h_to_b;
                fn->insts.data[hterm_id].targets[body_side].arg_count = nparams;
            }

            /* 3. Snapshot B's next_args and pop B's jmp H. */
            PirInstId bterm_id = fn->blocks.data[b].insts.data[fn->blocks.data[b].insts.len - 1u];
            PirOperand next_ops[8];
            for (uint32_t k = 0u; k < nparams; ++k) {
                next_ops[k] = fn->insts.data[bterm_id].targets[0].args[k];
            }
            fn->blocks.data[b].insts.len--;

            /* Materialize any non-VAL next_ops via PIR_OP_CONST so cloned H insts always have VALs. */
            PirValueId next_vals[8];
            for (uint32_t k = 0u; k < nparams; ++k) {
                if (next_ops[k].kind == PIR_OPER_VAL) {
                    next_vals[k] = next_ops[k].u.val;
                } else {
                    PirOpcode cop = (next_ops[k].kind == PIR_OPER_SYM) ? PIR_OP_ADDR : PIR_OP_CONST;
                    PirValueId cv = pir_fn_new_val(mod, fn, "lrc", h_ptys[k], b, PIR_INVALID_ID, 0u);
                    PirInst ci = pir_make_inst(cop, h_ptys[k], hterm_snap.src_line, hterm_snap.loc_idx);
                    ci.operands = pir_alloc_ops(mod, 1u);
                    ci.operands[0] = next_ops[k];
                    ci.operands[0].type = h_ptys[k];
                    ci.operand_count = 1u;
                    ci.results[0] = cv;
                    pir_fn_append_inst(fn, &fn->blocks.data[b].insts, ci);
                    next_vals[k] = cv;
                }
            }

            /* 4. Clone H's condition instructions into end of B with h_params -> next_vals. */
            PirValueId c_res[4];
            for (uint32_t j = 0u; j < n_alu; ++j) {
                PirInst ci = h_alu_snap[j];
                PirType rty = fn->values.data[h_res[j]].type;
                PirValueId rv = pir_fn_new_val(mod, fn, "lri", rty, b, PIR_INVALID_ID, 0u);
                c_res[j] = rv;
                ci.operands = ci.operand_count ? pir_alloc_ops(mod, ci.operand_count) : NULL;
                for (uint32_t opi = 0u; opi < ci.operand_count; ++opi) {
                    ci.operands[opi] = map_rot_op(h_alu_snap[j].operands[opi],
                                                  h_params, next_vals, nparams,
                                                  h_res, c_res, j);
                }
                ci.results[0] = rv;
                ci.results[1] = PIR_INVALID_ID;
                pir_fn_append_inst(fn, &fn->blocks.data[b].insts, ci);
            }

            /* 5. Emit rotated bottom-tested BR at end of B: back to B(next_ops) or exit E. */
            PirInst nbr = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID),
                                        hterm_snap.src_line, hterm_snap.loc_idx);
            nbr.operands = pir_alloc_ops(mod, 1u);
            nbr.operands[0] = map_rot_op(hterm_snap.operands[0],
                                         h_params, next_vals, nparams,
                                         h_res, c_res, n_alu);
            nbr.operand_count = 1u;

            nbr.targets[body_side].target    = b;
            nbr.targets[body_side].arg_count = nparams;
            if (nparams > 0u) {
                nbr.targets[body_side].args = pir_alloc_ops(mod, nparams);
                for (uint32_t k = 0u; k < nparams; ++k) {
                    nbr.targets[body_side].args[k] = next_ops[k];
                }
            }

            uint32_t e_argc = hterm_snap.targets[exit_side].arg_count;
            nbr.targets[exit_side].target    = e;
            nbr.targets[exit_side].arg_count = e_argc;
            if (e_argc > 0u) {
                nbr.targets[exit_side].args = pir_alloc_ops(mod, e_argc);
                for (uint32_t k = 0u; k < e_argc; ++k) {
                    nbr.targets[exit_side].args[k] = map_rot_op(
                        hterm_snap.targets[exit_side].args[k],
                        h_params, next_vals, nparams,
                        h_res, c_res, n_alu);
                }
            }
            pir_fn_append_inst(fn, &fn->blocks.data[b].insts, nbr);
            return true;
        }
    }
    return false;
}

void pir_opt_loop_rotate(PirModule *mod, PirFunction *fn) {
    if (fn->blocks.len < 2u) return;
    for (uint32_t iter = 0u; iter < 64u; ++iter) {
        PirCfg cfg;
        pir_cfg_build(&cfg, fn);
        bool changed = try_rotate_one_loop(mod, fn, &cfg);
        pir_cfg_destroy(&cfg);
        if (!changed) break;
    }
}
