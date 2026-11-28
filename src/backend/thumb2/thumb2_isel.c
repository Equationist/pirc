/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ARM Thumb-2 (ARMv7E-M, Cortex-M7 FPv5-D16 / Cortex-M3/M4) Instruction Selection (LIR -> MIR).
 *
 * All LIR values after i64 (+ optional soft-float) legalization are i8, i16, i32, ptr, f32, or f64.
 * GP vregs hold 32-bit values in r0..r12; FP vregs hold f32 (low lane s(2*k)) or f64 in d0..d15.
 */

#include "analysis/cfg.h"
#include "backend/backend.h"
#include "lower/lower.h"
#include "opt/opt.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const PirModule     *mod;
    const PirFunction   *fn;
    const PirTargetSpec *target;
    MFunc               *mf;
    PirDiag             *diag;
    bool                 ok;

    MVReg    *val_vreg;
    uint32_t *alloca_slot;
    uint32_t *blk_map;
    uint32_t  cur_mblk;

    /* Hidden multi-reg return buffer vreg (for PIR_CABI_RET_MULTI_REG). */
    MVReg     multi_ret_vreg;
} T2ISelCtx;

static bool t2_is_fp_type(PirType ty) {
    return ty.kind == PIR_TYPE_SCALAR &&
           (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64);
}

static uint8_t t2_val_width(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:  return 1u;
            case PIR_SCALAR_I16: return 2u;
            case PIR_SCALAR_F64: return 8u;
            case PIR_SCALAR_I32: case PIR_SCALAR_F32: case PIR_SCALAR_PTR:
            case PIR_SCALAR_VOID: case PIR_SCALAR_I64: case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST:
                return 4u;
        }
    }
    return 4u;
}

static void emit(T2ISelCtx *c, MInst ins) {
    mblock_push(&c->mf->blocks.data[c->cur_mblk], ins);
}

static MVReg new_gp(T2ISelCtx *c) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, 4u);
}

static MVReg new_fp(T2ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_FP, bytes);
}

static uint32_t eval_imm_u32(const PirOperand *op) {
    uint64_t raw = op->u.imm_int.is_negative ? (UINT64_C(0) - op->u.imm_int.lo) : op->u.imm_int.lo;
    return (uint32_t)(raw & 0xFFFFFFFFu);
}

static void emit_li(T2ISelCtx *c, MVReg dst, uint32_t val, uint32_t loc) {
    MInst li = minst_make(MI_T2_LI, 4u, loc);
    li.op_count = 2u;
    li.ops[0]   = mop_vreg_def(dst);
    li.ops[1]   = mop_imm((int64_t)(int32_t)val);
    emit(c, li);
}

static void emit_copy(T2ISelCtx *c, MVReg dst, MVReg src, uint8_t w, uint32_t loc) {
    MInst cp = minst_make(MI_COPY, w, loc);
    cp.op_count = 2u;
    cp.ops[0]   = mop_vreg_def(dst);
    cp.ops[1]   = mop_vreg_use(src);
    emit(c, cp);
}

static void emit_ext(T2ISelCtx *c, MVReg dst, MVReg src, uint8_t from_w, bool is_signed, uint32_t loc) {
    if (from_w >= 4u) {
        emit_copy(c, dst, src, 4u, loc);
        return;
    }
    MInst e = minst_make(MI_T2_EXT, from_w, loc);
    e.is_signed = is_signed;
    e.op_count  = 2u;
    e.ops[0]    = mop_vreg_def(dst);
    e.ops[1]    = mop_vreg_use(src);
    emit(c, e);
}

static void lookup_symbol_meta(const PirModule *mod, const char *name,
                               bool *out_is_func, bool *out_is_tls) {
    *out_is_func = false;
    *out_is_tls  = false;
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) {
            *out_is_func = true;
            return;
        }
    }
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, name) == 0) {
            *out_is_tls = (mod->globals.data[i].seg == PIR_GSEG_TLS);
            return;
        }
    }
}

static void emit_sym_addr(T2ISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls);
    MInst sa = minst_make(MI_T2_SYM_ADDR, 4u, loc);
    sa.op_count = 2u;
    sa.ops[0]   = mop_vreg_def(dst);
    sa.ops[1]   = mop_sym(name, is_func ? 0 : offset, is_func, is_tls, false);
    emit(c, sa);
    if (is_func && offset != 0) {
        MVReg t = new_gp(c);
        emit_copy(c, t, dst, 4u, loc);
        MInst ad = minst_make(MI_T2_ADDI, 4u, loc);
        ad.op_count = 3u;
        ad.ops[0] = mop_vreg_def(dst);
        ad.ops[1] = mop_vreg_use(t);
        ad.ops[2] = mop_imm(offset);
        emit(c, ad);
    }
}

static void emit_load_imm_fp(T2ISelCtx *c, MVReg dst, uint64_t bits, uint8_t w, uint32_t loc) {
    MInst lf = minst_make(MI_T2_LOAD_IMM_FP, w, loc);
    lf.op_count = 2u;
    lf.ops[0]   = mop_vreg_def(dst);
    lf.ops[1]   = mop_imm((int64_t)bits);
    emit(c, lf);
}

static MVReg mat_op(T2ISelCtx *c, const PirOperand *op, uint32_t loc) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            return c->val_vreg[op->u.val];
        case PIR_OPER_IMM_INT: {
            MVReg r = new_gp(c);
            emit_li(c, r, eval_imm_u32(op), loc);
            return r;
        }
        case PIR_OPER_IMM_FLOAT: {
            uint8_t w = (op->type.kind == PIR_TYPE_SCALAR && op->type.u.scalar == PIR_SCALAR_F64) ? 8u : 4u;
            MVReg r = new_fp(c, w);
            emit_load_imm_fp(c, r, op->u.imm_float.bits, w, loc);
            return r;
        }
        case PIR_OPER_SYM: {
            MVReg r = new_gp(c);
            emit_sym_addr(c, r, op->u.sym.name, op->u.sym.offset, loc);
            return r;
        }
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            break;
    }
    return new_gp(c);
}

static MVReg mat_ext(T2ISelCtx *c, const PirOperand *op, uint8_t w, bool is_signed, uint32_t loc) {
    MVReg v = mat_op(c, op, loc);
    if (w >= 4u) return v;
    MVReg e = new_gp(c);
    emit_ext(c, e, v, w, is_signed, loc);
    return e;
}

static void emit_trap_if_nz(T2ISelCtx *c, MVReg cond, uint32_t loc) {
    uint32_t trap_mb = mfunc_new_block(c->mf, NULL);
    uint32_t cont_mb = mfunc_new_block(c->mf, NULL);

    MInst br = minst_make(MI_T2_BR_NZ, 4u, loc);
    br.op_count = 2u; br.ops[0] = mop_vreg_use(cond); br.ops[1] = mop_block(trap_mb);
    emit(c, br);

    MInst jc = minst_make(MI_T2_B, 4u, loc);
    jc.op_count = 1u; jc.ops[0] = mop_block(cont_mb);
    emit(c, jc);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_mb);
    mfunc_add_edge(c->mf, c->cur_mblk, cont_mb);

    c->cur_mblk = trap_mb;
    emit(c, minst_make(MI_T2_TRAP, 4u, loc));

    c->cur_mblk = cont_mb;
}

static void emit_trap_if_cmp(T2ISelCtx *c, MCond cc, MVReg lhs, MVReg rhs, uint32_t loc) {
    uint32_t trap_mb = mfunc_new_block(c->mf, NULL);
    uint32_t cont_mb = mfunc_new_block(c->mf, NULL);

    MInst br = minst_make(MI_T2_BC, 4u, loc);
    br.op_count = 4u;
    br.ops[0] = mop_vreg_use(lhs);
    br.ops[1] = mop_vreg_use(rhs);
    br.ops[2] = mop_cond(cc);
    br.ops[3] = mop_block(trap_mb);
    emit(c, br);

    MInst jc = minst_make(MI_T2_B, 4u, loc);
    jc.op_count = 1u; jc.ops[0] = mop_block(cont_mb);
    emit(c, jc);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_mb);
    mfunc_add_edge(c->mf, c->cur_mblk, cont_mb);

    c->cur_mblk = trap_mb;
    emit(c, minst_make(MI_T2_TRAP, 4u, loc));

    c->cur_mblk = cont_mb;
}

/* =========================================================================
 * AAPCS32 / AAPCS32-VFP Argument Placement Planner (Shared: Entry & Calls)
 * ========================================================================= */

typedef struct {
    int8_t  gp_reg;     /* 0..3 if GP arg (or lo word of variadic f64), else -1 */
    int8_t  gp_reg_hi;  /* 1 or 3 for variadic/soft f64 in GP reg pair, else -1 */
    int8_t  vfp_s;      /* 0..15 for hard-float non-variadic f32/f64 (f64 uses even), else -1 */
    int64_t soff;       /* Non-negative stack byte offset if on stack, else -1 */
    uint8_t byte_size;  /* 4 or 8 */
} T2ArgLoc;

static uint64_t t2_plan_args(uint32_t n, bool is_variadic, bool has_hw_fp,
                             const bool *is_fp, const uint8_t *widths,
                             const uint32_t *stk_aligns, const bool *force_stack,
                             const uint8_t *group_words, T2ArgLoc *locs,
                             uint32_t *out_gp_used) {
    uint32_t gp_used = 0u;
    uint32_t s_mask  = 0u;
    bool     vfp_spilled = false;
    uint64_t stk_off = 0u;

    for (uint32_t i = 0u; i < n; ) {
        locs[i].gp_reg    = -1;
        locs[i].gp_reg_hi = -1;
        locs[i].vfp_s     = -1;
        locs[i].soff      = -1;
        locs[i].byte_size = (widths[i] == 8u) ? 8u : 4u;

        bool use_vfp = has_hw_fp && !is_variadic && is_fp[i] && (!force_stack || !force_stack[i]);
        if (use_vfp) {
            uint32_t gw = (group_words && group_words[i] >= 2u && i + group_words[i] <= n)
                          ? (uint32_t)group_words[i] : 1u;
            uint32_t slots_per = (widths[i] == 8u) ? 2u : 1u;
            uint32_t tot_slots = gw * slots_per;
            int8_t found_s = -1;
            if (!vfp_spilled && tot_slots <= 16u) {
                uint32_t need = (1u << tot_slots) - 1u;
                for (uint32_t s = 0u; s + tot_slots <= 16u; s += slots_per) {
                    if ((s_mask & (need << s)) == 0u) {
                        s_mask |= (need << s);
                        found_s = (int8_t)s;
                        break;
                    }
                }
            }
            if (found_s >= 0) {
                for (uint32_t k = 0u; k < gw; ++k) {
                    locs[i + k].gp_reg    = -1;
                    locs[i + k].gp_reg_hi = -1;
                    locs[i + k].vfp_s     = (int8_t)((uint32_t)found_s + k * slots_per);
                    locs[i + k].soff      = -1;
                    locs[i + k].byte_size = (widths[i + k] == 8u) ? 8u : 4u;
                }
            } else {
                vfp_spilled = true;
                uint64_t al = (slots_per == 2u) ? 8u : 4u;
                stk_off = (stk_off + al - 1u) & ~(al - 1u);
                for (uint32_t k = 0u; k < gw; ++k) {
                    uint64_t sz = (widths[i + k] == 8u) ? 8u : 4u;
                    locs[i + k].gp_reg    = -1;
                    locs[i + k].gp_reg_hi = -1;
                    locs[i + k].vfp_s     = -1;
                    locs[i + k].soff      = (int64_t)stk_off;
                    locs[i + k].byte_size = (uint8_t)sz;
                    stk_off += sz;
                }
            }
            i += gw;
            continue;
        }

        if (is_fp[i] && !is_variadic && force_stack && force_stack[i]) {
            /* Spilled HFA elements forced directly to stack without consuming GP regs. */
            uint64_t sz = (widths[i] == 8u) ? 8u : 4u;
            uint64_t al = (stk_aligns && stk_aligns[i] >= 8u) ? 8u : sz;
            stk_off = (stk_off + al - 1u) & ~(al - 1u);
            locs[i].soff = (int64_t)stk_off;
            stk_off += sz;
            i++;
            continue;
        }

        /* GP (or variadic/soft-float FP) argument */
        bool need_al8 = (widths[i] == 8u) || (stk_aligns && stk_aligns[i] >= 8u);
        bool fstk = (force_stack && force_stack[i]);

        if (!fstk && widths[i] == 8u) {
            /* 8-byte scalar (variadic f64): even GP pair r0:r1 or r2:r3, else 8B stack */
            if ((gp_used & 1u) != 0u) gp_used++;
            if (gp_used + 2u <= 4u) {
                locs[i].gp_reg    = (int8_t)gp_used;
                locs[i].gp_reg_hi = (int8_t)(gp_used + 1u);
                gp_used += 2u;
                i++;
                continue;
            }
            gp_used = 4u;
        } else if (!fstk) {
            if (need_al8 && (gp_used & 1u) != 0u) gp_used++;
            if (gp_used < 4u) {
                /* For 8B-aligned split i64 (two consecutive i32s where first has stk_align=8),
                 * ensure we never split across r3 + stack (round to stack if gp_used == 3). */
                locs[i].gp_reg = (int8_t)gp_used;
                gp_used++;
                i++;
                continue;
            }
        }

        uint64_t sz = (widths[i] == 8u) ? 8u : 4u;
        uint64_t al = need_al8 ? 8u : 4u;
        stk_off = (stk_off + al - 1u) & ~(al - 1u);
        locs[i].soff = (int64_t)stk_off;
        stk_off += sz;
        i++;
    }

    if (out_gp_used) *out_gp_used = gp_used;
    return (stk_off + 7u) & ~UINT64_C(7);
}

/* =========================================================================
 * Block-Parameter Parallel Copies & Edge Splitting
 * ========================================================================= */

static void emit_branch_pcopy(T2ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    if (bt->arg_count == 0u) return;
    const PirBlock *dst_blk = &c->fn->blocks.data[bt->target];
    MPCopyEntry *ents = (MPCopyEntry *)calloc(bt->arg_count, sizeof(MPCopyEntry));
    if (!ents) abort();
    uint32_t cnt = 0u;

    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        PirValueId dv = dst_blk->params.data[k];
        const PirOperand *aop = &bt->args[k];
        MVReg dvreg = c->val_vreg[dv];
        const MVRegInfo *dvi = &c->mf->vregs.data[dvreg];
        ents[cnt].dst       = dvreg;
        ents[cnt].rclass    = dvi->rclass;
        ents[cnt].byte_size = dvi->byte_size;
        if (aop->kind == PIR_OPER_IMM_INT && dvi->rclass == MREG_CLASS_GP) {
            ents[cnt].src_is_imm = true;
            ents[cnt].imm_bits   = (uint64_t)eval_imm_u32(aop);
        } else if (aop->kind == PIR_OPER_IMM_FLOAT && dvi->rclass == MREG_CLASS_FP) {
            ents[cnt].src_is_imm = true;
            ents[cnt].imm_bits   = aop->u.imm_float.bits;
        } else {
            ents[cnt].src_is_imm = false;
            ents[cnt].src        = mat_op(c, aop, loc);
        }
        cnt++;
    }
    if (cnt > 0u) {
        uint32_t pid = mfunc_add_pcopy(c->mf, ents, cnt);
        MInst pc = minst_make(MI_PCOPY, 4u, loc);
        pc.aux_id = pid;
        emit(c, pc);
    }
    free(ents);
}

static uint32_t resolve_edge_target(T2ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    uint32_t dst_mb = c->blk_map[bt->target];
    if (bt->arg_count == 0u) return dst_mb;
    uint32_t tramp = mfunc_new_block(c->mf, NULL);
    uint32_t saved = c->cur_mblk;
    c->cur_mblk = tramp;
    emit_branch_pcopy(c, bt, loc);
    MInst j = minst_make(MI_T2_B, 4u, loc);
    j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
    emit(c, j);
    mfunc_add_edge(c->mf, tramp, dst_mb);
    c->cur_mblk = saved;
    return tramp;
}

/* =========================================================================
 * Calls (Direct, Indirect, Multi-Reg Returns, VFP s0..s15 Packing)
 * ========================================================================= */

static void isel_call(T2ISelCtx *c, const PirInst *inst, uint32_t loc) {
    c->mf->has_calls = true;
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);

    PirCabiRetInfo cabi_ret;
    memset(&cabi_ret, 0, sizeof(cabi_ret));
    bool is_variadic = false;
    const PirParamDecl *callee_params = NULL;
    uint32_t callee_param_count = 0u;

    if (!is_ind && inst->operands[0].u.sym.is_func &&
        inst->operands[0].u.sym.id < c->mod->funcs.len) {
        const PirFunction *cal = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        cabi_ret           = cal->cabi_ret;
        is_variadic        = cal->is_variadic;
        callee_params      = cal->params;
        callee_param_count = cal->param_count;
    } else if (is_ind && inst->sig_id < c->mod->sigs.len) {
        const PirSigDef *sig = &c->mod->sigs.data[inst->sig_id];
        cabi_ret           = sig->cabi_ret;
        is_variadic        = sig->is_variadic;
        callee_params      = sig->params;
        callee_param_count = sig->param_count;
    }

    uint32_t first_arg = 1u;
    MVReg multi_ret_buf = MVREG_NONE;
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && inst->operand_count >= 2u) {
        multi_ret_buf = mat_op(c, &inst->operands[1], loc);
        first_arg = 2u;
    }

    uint32_t nargs = (inst->operand_count > first_arg) ? (inst->operand_count - first_arg) : 0u;
    MVReg    *arg_v   = nargs ? (MVReg *)calloc(nargs, sizeof(MVReg)) : NULL;
    bool     *arg_fp  = nargs ? (bool *)calloc(nargs, sizeof(bool)) : NULL;
    uint8_t  *arg_w   = nargs ? (uint8_t *)calloc(nargs, sizeof(uint8_t)) : NULL;
    uint32_t *arg_al  = nargs ? (uint32_t *)calloc(nargs, sizeof(uint32_t)) : NULL;
    bool     *arg_fs  = nargs ? (bool *)calloc(nargs, sizeof(bool)) : NULL;
    uint8_t  *arg_gw  = nargs ? (uint8_t *)calloc(nargs, sizeof(uint8_t)) : NULL;
    T2ArgLoc *locs    = nargs ? (T2ArgLoc *)calloc(nargs, sizeof(T2ArgLoc)) : NULL;
    if (nargs && (!arg_v || !arg_fp || !arg_w || !arg_al || !arg_fs || !arg_gw || !locs)) abort();

    for (uint32_t i = 0u; i < nargs; ++i) {
        const PirOperand *aop = &inst->operands[first_arg + i];
        arg_v[i]  = mat_op(c, aop, loc);
        arg_fp[i] = t2_is_fp_type(aop->type);
        arg_w[i]  = t2_val_width(aop->type);

        uint32_t pi = (first_arg - 1u) + i;
        uint32_t al = aop->call_stack_align;
        if (al == 0u && callee_params && pi < callee_param_count) al = callee_params[pi].abi_stack_align;
        arg_al[i] = al;

        bool fs = aop->call_force_stack ||
                  (callee_params && pi < callee_param_count && callee_params[pi].abi_force_stack);
        arg_fs[i] = fs;

        uint8_t gw = aop->call_group_words;
        if (gw == 0u && callee_params && pi < callee_param_count) gw = callee_params[pi].abi_group_words;
        arg_gw[i] = gw;

        /* C variadic default promotion: f32 -> f64 in variadic argument positions. */
        if (is_variadic && pi >= callee_param_count && arg_fp[i] && arg_w[i] == 4u &&
            !aop->call_no_f32_promote) {
            MVReg dv = new_fp(c, 8u);
            MInst cv = minst_make(MI_T2_FCVT_F2F, 8u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dv); cv.ops[1] = mop_vreg_use(arg_v[i]);
            emit(c, cv);
            arg_v[i]  = dv;
            arg_w[i]  = 8u;
            arg_al[i] = 8u;
        }

        /* Extend i8/i16 args on C calls according to signext/zeroext (default zeroext). */
        if (!arg_fp[i] && arg_w[i] < 4u) {
            bool sext = (callee_params && pi < callee_param_count &&
                         (callee_params[pi].attrs & PIR_PATTR_SIGNEXT) != 0u);
            MVReg wv = new_gp(c);
            emit_ext(c, wv, arg_v[i], arg_w[i], sext, loc);
            arg_v[i] = wv;
            arg_w[i] = 4u;
        }
    }

    uint64_t out_bytes = t2_plan_args(nargs, is_variadic, c->target->caps.has_hw_fp,
                                      arg_fp, arg_w, arg_al, arg_fs, arg_gw, locs, NULL);
    if (out_bytes > c->mf->max_out_args_bytes) c->mf->max_out_args_bytes = out_bytes;

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_DOWN, 4u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    /* Emit outgoing stack arg stores first. */
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (locs[i].soff < 0) continue;
        MInst st = minst_make(MI_STORE_OUTARG, locs[i].byte_size, loc);
        st.op_count = 2u;
        st.ops[0]   = mop_vreg_use(arg_v[i]);
        st.ops[1]   = mop_imm(locs[i].soff);
        emit(c, st);
    }

    MVReg ind_v = MVREG_NONE;
    if (is_ind) {
        ind_v = mat_op(c, &inst->operands[0], loc);
    }

    /* Build parallel copy entries for GP (r0..r3) and VFP (d0..d7) register args. */
    MPCopyEntry *ents = (MPCopyEntry *)calloc(nargs * 2u + 8u, sizeof(MPCopyEntry));
    if (!ents) abort();
    uint32_t nents = 0u;

    /* 1. GP register arguments (including variadic f32/f64 converted to GP!) */
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (locs[i].gp_reg < 0) continue;
        if (arg_fp[i] && arg_w[i] == 8u && locs[i].gp_reg_hi >= 0) {
            MVReg lo_g = new_gp(c), hi_g = new_gp(c);
            MInst mv = minst_make(MI_T2_VMOV_D2G, 8u, loc);
            mv.op_count = 3u;
            mv.ops[0] = mop_vreg_def(lo_g);
            mv.ops[1] = mop_vreg_def(hi_g);
            mv.ops[2] = mop_vreg_use(arg_v[i]);
            emit(c, mv);

            MVReg f0 = new_gp(c); c->mf->vregs.data[f0].fixed_preg = T2_GP((uint32_t)locs[i].gp_reg);
            MVReg f1 = new_gp(c); c->mf->vregs.data[f1].fixed_preg = T2_GP((uint32_t)locs[i].gp_reg_hi);
            ents[nents].dst = f0; ents[nents].src = lo_g; ents[nents].rclass = MREG_CLASS_GP; ents[nents].byte_size = 4u; nents++;
            ents[nents].dst = f1; ents[nents].src = hi_g; ents[nents].rclass = MREG_CLASS_GP; ents[nents].byte_size = 4u; nents++;
        } else if (arg_fp[i] && arg_w[i] == 4u) {
            MVReg g = new_gp(c);
            MInst mv = minst_make(MI_T2_VMOV_S2G, 4u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(g); mv.ops[1] = mop_vreg_use(arg_v[i]);
            emit(c, mv);

            MVReg f0 = new_gp(c); c->mf->vregs.data[f0].fixed_preg = T2_GP((uint32_t)locs[i].gp_reg);
            ents[nents].dst = f0; ents[nents].src = g; ents[nents].rclass = MREG_CLASS_GP; ents[nents].byte_size = 4u; nents++;
        } else {
            MVReg f0 = new_gp(c); c->mf->vregs.data[f0].fixed_preg = T2_GP((uint32_t)locs[i].gp_reg);
            ents[nents].dst = f0; ents[nents].src = arg_v[i]; ents[nents].rclass = MREG_CLASS_GP; ents[nents].byte_size = 4u; nents++;
        }
    }

    /* 2. VFP register arguments: group by d0..d7 and pack any even+odd s-register pairs! */
    MVReg d_f64[8];
    MVReg d_s_lo[8], d_s_hi[8];
    for (uint32_t d = 0u; d < 8u; ++d) {
        d_f64[d] = MVREG_NONE;
        d_s_lo[d] = MVREG_NONE;
        d_s_hi[d] = MVREG_NONE;
    }
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (locs[i].vfp_s < 0) continue;
        uint32_t s = (uint32_t)locs[i].vfp_s;
        uint32_t d = s / 2u;
        if (arg_w[i] == 8u) {
            d_f64[d] = arg_v[i];
        } else if ((s & 1u) == 0u) {
            d_s_lo[d] = arg_v[i];
        } else {
            d_s_hi[d] = arg_v[i];
        }
    }
    for (uint32_t d = 0u; d < 8u; ++d) {
        if (d_f64[d] != MVREG_NONE) {
            MVReg fd = new_fp(c, 8u); c->mf->vregs.data[fd].fixed_preg = T2_D(d);
            ents[nents].dst = fd; ents[nents].src = d_f64[d]; ents[nents].rclass = MREG_CLASS_FP; ents[nents].byte_size = 8u; nents++;
        } else if (d_s_hi[d] != MVREG_NONE) {
            MVReg lo_v = (d_s_lo[d] != MVREG_NONE) ? d_s_lo[d] : d_s_hi[d];
            MVReg pk = new_fp(c, 8u);
            MInst pm = minst_make(MI_T2_PACK_S2, 8u, loc);
            pm.op_count = 3u;
            pm.ops[0] = mop_vreg_def_ec(pk);
            pm.ops[1] = mop_vreg_use(lo_v);
            pm.ops[2] = mop_vreg_use(d_s_hi[d]);
            emit(c, pm);

            MVReg fd = new_fp(c, 8u); c->mf->vregs.data[fd].fixed_preg = T2_D(d);
            ents[nents].dst = fd; ents[nents].src = pk; ents[nents].rclass = MREG_CLASS_FP; ents[nents].byte_size = 8u; nents++;
        } else if (d_s_lo[d] != MVREG_NONE) {
            MVReg fd = new_fp(c, 4u); c->mf->vregs.data[fd].fixed_preg = T2_D(d);
            ents[nents].dst = fd; ents[nents].src = d_s_lo[d]; ents[nents].rclass = MREG_CLASS_FP; ents[nents].byte_size = 4u; nents++;
        }
    }

    if (nents > 0u) {
        uint32_t pid = mfunc_add_pcopy(c->mf, ents, nents);
        MInst pc = minst_make(MI_PCOPY, 4u, loc);
        pc.aux_id = pid;
        emit(c, pc);
    }
    free(ents);

    if (is_ind) {
        MInst ci = minst_make(MI_T2_CALL_IND, 4u, loc);
        ci.is_call  = true;
        ci.op_count = 1u;
        ci.ops[0]   = mop_vreg_use(ind_v);
        emit(c, ci);
    } else {
        MInst cl = minst_make(MI_T2_CALL, 4u, loc);
        cl.is_call  = true;
        cl.op_count = 1u;
        cl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, cl);
    }

    /* Capture return value(s) BEFORE restoring stack! */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && multi_ret_buf != MVREG_NONE) {
        uint32_t rc = cabi_ret.reg_count;
        if (rc > 4u) rc = 4u;
        bool ret_fp = (rc > 0u && t2_is_fp_type(cabi_ret.regs[0]));
        if (ret_fp && cabi_ret.regs[0].u.scalar == PIR_SCALAR_F32) {
            /* HFA of 2..4 f32 returned in s0..s(rc-1) (packed in d0, d1)! */
            uint32_t nd = (rc + 1u) / 2u;
            MVReg raw_d[2] = { MVREG_NONE, MVREG_NONE };
            for (uint32_t d = 0u; d < nd; ++d) {
                raw_d[d] = new_fp(c, 8u);
                MInst cp = minst_make(MI_COPY, 8u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(raw_d[d]); cp.ops[1] = mop_preg_use(T2_D(d));
                emit(c, cp);
            }
            for (uint32_t k = 0u; k < rc; ++k) {
                MVReg fv = new_fp(c, 4u);
                MInst up = minst_make(MI_T2_UNPACK_S, 4u, loc);
                up.aux_width = (uint8_t)(k & 1u);
                up.op_count  = 2u; up.ops[0] = mop_vreg_def(fv); up.ops[1] = mop_vreg_use(raw_d[k / 2u]);
                emit(c, up);

                MInst st = minst_make(MI_T2_VST, 4u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(fv);
                st.ops[1] = mop_vreg_use(multi_ret_buf);
                st.ops[2] = mop_imm((int64_t)(k * 4u));
                emit(c, st);
            }
        } else if (ret_fp) {
            /* HFA of 2..4 f64 returned in d0..d(rc-1) */
            MVReg rvs[4];
            for (uint32_t k = 0u; k < rc; ++k) {
                rvs[k] = new_fp(c, 8u);
                MInst cp = minst_make(MI_COPY, 8u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(rvs[k]); cp.ops[1] = mop_preg_use(T2_D(k));
                emit(c, cp);
            }
            for (uint32_t k = 0u; k < rc; ++k) {
                MInst st = minst_make(MI_T2_VST, 8u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(rvs[k]);
                st.ops[1] = mop_vreg_use(multi_ret_buf);
                st.ops[2] = mop_imm((int64_t)(k * 8u));
                emit(c, st);
            }
        } else {
            /* Multi-word GP return (e.g. i64 in r0:r1) */
            MVReg rvs[4];
            for (uint32_t k = 0u; k < rc; ++k) {
                rvs[k] = new_gp(c);
                MInst cp = minst_make(MI_COPY, 4u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(rvs[k]); cp.ops[1] = mop_preg_use(T2_GP(k));
                emit(c, cp);
            }
            for (uint32_t k = 0u; k < rc; ++k) {
                MInst st = minst_make(MI_T2_ST, 4u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(rvs[k]);
                st.ops[1] = mop_vreg_use(multi_ret_buf);
                st.ops[2] = mop_imm((int64_t)(k * 4u));
                emit(c, st);
            }
        }
    } else if (inst->results[0] != PIR_INVALID_ID) {
        MVReg dst = c->val_vreg[inst->results[0]];
        uint8_t rw = t2_val_width(inst->ty);
        if (t2_is_fp_type(inst->ty)) {
            if (is_variadic || !c->target->caps.has_hw_fp) {
                if (rw == 8u) {
                    MVReg lo = new_gp(c), hi = new_gp(c);
                    MInst c0 = minst_make(MI_COPY, 4u, loc);
                    c0.op_count = 2u; c0.ops[0] = mop_vreg_def(lo); c0.ops[1] = mop_preg_use(T2_R0);
                    emit(c, c0);
                    MInst c1 = minst_make(MI_COPY, 4u, loc);
                    c1.op_count = 2u; c1.ops[0] = mop_vreg_def(hi); c1.ops[1] = mop_preg_use(T2_R1);
                    emit(c, c1);
                    MInst mv = minst_make(MI_T2_VMOV_G2D, 8u, loc);
                    mv.op_count = 3u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(lo); mv.ops[2] = mop_vreg_use(hi);
                    emit(c, mv);
                } else {
                    MVReg g = new_gp(c);
                    MInst c0 = minst_make(MI_COPY, 4u, loc);
                    c0.op_count = 2u; c0.ops[0] = mop_vreg_def(g); c0.ops[1] = mop_preg_use(T2_R0);
                    emit(c, c0);
                    MInst mv = minst_make(MI_T2_VMOV_G2S, 4u, loc);
                    mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(g);
                    emit(c, mv);
                }
            } else {
                MInst cp = minst_make(MI_COPY, rw, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(T2_D(0));
                emit(c, cp);
            }
        } else {
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(T2_R0);
            emit(c, cp);
        }
    }

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_UP, 4u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    free(arg_v); free(arg_fp); free(arg_w); free(arg_al); free(arg_fs); free(arg_gw); free(locs);
}

/* =========================================================================
 * Integer Division & Remainder (Hardware Thumb-2 sdiv / udiv)
 * ========================================================================= */

static void isel_div_rem(T2ISelCtx *c, const PirInst *inst, uint32_t loc) {
    uint8_t w = t2_val_width(inst->ty);
    bool is_s = (inst->op == PIR_OP_DIV_S || inst->op == PIR_OP_REM_S);
    bool is_rem = (inst->op == PIR_OP_REM_S || inst->op == PIR_OP_REM_U);
    MVReg dst = c->val_vreg[inst->results[0]];

    MVReg a = mat_ext(c, &inst->operands[0], w, is_s, loc);
    MVReg b = mat_ext(c, &inst->operands[1], w, is_s, loc);

    /* Guard against divide-by-zero (always trap). */
    MVReg zero_r = new_gp(c);
    emit_li(c, zero_r, 0u, loc);
    emit_trap_if_cmp(c, MCOND_EQ, b, zero_r, loc);

    MVReg is_min_m1 = MVREG_NONE;
    MVReg call_b = b;
    if (is_s) {
        uint32_t min_v = (w == 1u) ? 0xFFFFFF80u : (w == 2u) ? 0xFFFF8000u : 0x80000000u;
        MVReg min_r = new_gp(c); emit_li(c, min_r, min_v, loc);
        MVReg m1_r  = new_gp(c); emit_li(c, m1_r, 0xFFFFFFFFu, loc);

        MVReg eq_min = new_gp(c);
        MInst c1 = minst_make(MI_T2_CMP_SET, 4u, loc);
        c1.op_count = 4u; c1.ops[0] = mop_vreg_def(eq_min);
        c1.ops[1] = mop_vreg_use(a); c1.ops[2] = mop_vreg_use(min_r); c1.ops[3] = mop_cond(MCOND_EQ);
        emit(c, c1);

        MVReg eq_m1 = new_gp(c);
        MInst c2 = minst_make(MI_T2_CMP_SET, 4u, loc);
        c2.op_count = 4u; c2.ops[0] = mop_vreg_def(eq_m1);
        c2.ops[1] = mop_vreg_use(b); c2.ops[2] = mop_vreg_use(m1_r); c2.ops[3] = mop_cond(MCOND_EQ);
        emit(c, c2);

        is_min_m1 = new_gp(c);
        MInst an = minst_make(MI_T2_AND, 4u, loc);
        an.op_count = 3u; an.ops[0] = mop_vreg_def(is_min_m1);
        an.ops[1] = mop_vreg_use(eq_min); an.ops[2] = mop_vreg_use(eq_m1);
        emit(c, an);

        if (inst->flags & PIR_FLAG_WRAP) {
            MVReg one_r = new_gp(c); emit_li(c, one_r, 1u, loc);
            call_b = new_gp(c);
            MInst sl = minst_make(MI_T2_SELECT, 4u, loc);
            sl.op_count = 4u; sl.ops[0] = mop_vreg_def(call_b);
            sl.ops[1] = mop_vreg_use(is_min_m1); sl.ops[2] = mop_vreg_use(one_r); sl.ops[3] = mop_vreg_use(b);
            emit(c, sl);
        } else {
            emit_trap_if_nz(c, is_min_m1, loc);
        }
    }

    MVReg q = is_rem ? new_gp(c) : ((is_s && (inst->flags & PIR_FLAG_WRAP)) ? new_gp(c) : dst);
    MInst dv = minst_make(MI_T2_DIV, 4u, loc);
    dv.is_signed = is_s;
    dv.op_count  = 3u;
    dv.ops[0] = mop_vreg_def(q);
    dv.ops[1] = mop_vreg_use(a);
    dv.ops[2] = mop_vreg_use(call_b);
    emit(c, dv);

    MVReg raw = q;
    if (is_rem) {
        MVReg prod = new_gp(c);
        MInst ml = minst_make(MI_T2_MUL, 4u, loc);
        ml.op_count = 3u; ml.ops[0] = mop_vreg_def(prod); ml.ops[1] = mop_vreg_use(q); ml.ops[2] = mop_vreg_use(call_b);
        emit(c, ml);

        raw = (is_s && (inst->flags & PIR_FLAG_WRAP)) ? new_gp(c) : dst;
        MInst sb = minst_make(MI_T2_SUB, 4u, loc);
        sb.op_count = 3u; sb.ops[0] = mop_vreg_def(raw); sb.ops[1] = mop_vreg_use(a); sb.ops[2] = mop_vreg_use(prod);
        emit(c, sb);
    }

    if (is_s && (inst->flags & PIR_FLAG_WRAP) && is_min_m1 != MVREG_NONE) {
        uint32_t wv_imm = is_rem ? 0u : ((w == 1u) ? 0xFFFFFF80u : (w == 2u) ? 0xFFFF8000u : 0x80000000u);
        MVReg wv = new_gp(c); emit_li(c, wv, wv_imm, loc);
        MInst sl = minst_make(MI_T2_SELECT, 4u, loc);
        sl.op_count = 4u; sl.ops[0] = mop_vreg_def(dst);
        sl.ops[1] = mop_vreg_use(is_min_m1); sl.ops[2] = mop_vreg_use(wv); sl.ops[3] = mop_vreg_use(raw);
        emit(c, sl);
    }
}

/* =========================================================================
 * Main Instruction Selection Dispatcher
 * ========================================================================= */

static MCond pir_cmp_to_mcond(PirOpcode op) {
    if (op == PIR_OP_CMP_EQ)   return MCOND_EQ;
    if (op == PIR_OP_CMP_NE)   return MCOND_NE;
    if (op == PIR_OP_CMP_LT_S) return MCOND_LT;
    if (op == PIR_OP_CMP_LE_S) return MCOND_LE;
    if (op == PIR_OP_CMP_GT_S) return MCOND_GT;
    if (op == PIR_OP_CMP_GE_S) return MCOND_GE;
    if (op == PIR_OP_CMP_LT_U) return MCOND_CC;
    if (op == PIR_OP_CMP_LE_U) return MCOND_LS;
    if (op == PIR_OP_CMP_GT_U) return MCOND_HI;
    if (op == PIR_OP_CMP_GE_U) return MCOND_CS;
    return MCOND_EQ;
}

static void isel_inst(T2ISelCtx *c, PirInstId iid, const PirInst *inst) {
    uint32_t loc = inst->loc_idx;

    switch (inst->op) {
        case PIR_OP_CONST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            if (inst->operands[0].kind == PIR_OPER_SYM) {
                emit_sym_addr(c, dst, inst->operands[0].u.sym.name,
                              inst->operands[0].u.sym.offset, loc);
            } else if (t2_is_fp_type(inst->ty)) {
                emit_load_imm_fp(c, dst, inst->operands[0].u.imm_float.bits,
                                 t2_val_width(inst->ty), loc);
            } else {
                emit_li(c, dst, eval_imm_u32(&inst->operands[0]), loc);
            }
            return;
        }

        case PIR_OP_ADDR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            emit_sym_addr(c, dst, inst->operands[0].u.sym.name, inst->operands[0].u.sym.offset, loc);
            return;
        }

        case PIR_OP_ALLOCA_PTR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
            fa.op_count = 3u;
            fa.ops[0] = mop_vreg_def(dst);
            fa.ops[1] = mop_slot(c->alloca_slot[iid]);
            fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return;
        }

        case PIR_OP_ALLOCA_DYN: {
            c->mf->has_alloca_dyn = true;
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg sz  = mat_op(c, &inst->operands[0], loc);
            uint32_t al = (inst->align > 8u) ? inst->align : 8u;
            if ((al & (al - 1u)) != 0u) al = 8u;

            /* aligned_sz = (sz + al - 1) & ~(al - 1) */
            MVReg t1 = new_gp(c);
            MInst a1 = minst_make(MI_T2_ADDI, 4u, loc);
            a1.op_count = 3u; a1.ops[0] = mop_vreg_def(t1);
            a1.ops[1] = mop_vreg_use(sz); a1.ops[2] = mop_imm((int64_t)(al - 1u));
            emit(c, a1);

            MVReg mask_r = new_gp(c);
            emit_li(c, mask_r, (uint32_t)(0u - al), loc);
            MVReg asz = new_gp(c);
            MInst an = minst_make(MI_T2_AND, 4u, loc);
            an.op_count = 3u; an.ops[0] = mop_vreg_def(asz);
            an.ops[1] = mop_vreg_use(t1); an.ops[2] = mop_vreg_use(mask_r);
            emit(c, an);

            MVReg sp0 = new_gp(c);
            MInst m0 = minst_make(MI_T2_MOV, 4u, loc);
            m0.op_count = 2u; m0.ops[0] = mop_vreg_def(sp0); m0.ops[1] = mop_preg_use(T2_SP);
            emit(c, m0);

            MVReg sp1 = new_gp(c);
            MInst sb = minst_make(MI_T2_SUB, 4u, loc);
            sb.op_count = 3u; sb.ops[0] = mop_vreg_def(sp1);
            sb.ops[1] = mop_vreg_use(sp0); sb.ops[2] = mop_vreg_use(asz);
            emit(c, sb);

            if (al > 8u) {
                MVReg sp2 = new_gp(c);
                MInst aln = minst_make(MI_T2_AND, 4u, loc);
                aln.op_count = 3u; aln.ops[0] = mop_vreg_def(sp2);
                aln.ops[1] = mop_vreg_use(sp1); aln.ops[2] = mop_vreg_use(mask_r);
                emit(c, aln);
                sp1 = sp2;
            }

            MInst wsp = minst_make(MI_T2_MOV, 4u, loc);
            wsp.op_count = 2u; wsp.ops[0] = mop_preg_def(T2_SP); wsp.ops[1] = mop_vreg_use(sp1);
            emit(c, wsp);
            emit_copy(c, dst, sp1, 4u, loc);
            return;
        }

        case PIR_OP_STACKSAVE: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst m = minst_make(MI_T2_MOV, 4u, loc);
            m.op_count = 2u; m.ops[0] = mop_vreg_def(dst); m.ops[1] = mop_preg_use(T2_SP);
            emit(c, m);
            return;
        }

        case PIR_OP_STACKRESTORE: {
            MVReg s = mat_op(c, &inst->operands[0], loc);
            MInst m = minst_make(MI_T2_MOV, 4u, loc);
            m.op_count = 2u; m.ops[0] = mop_preg_def(T2_SP); m.ops[1] = mop_vreg_use(s);
            emit(c, m);
            return;
        }

        case PIR_OP_VA_START: {
            MVReg ap_ptr = mat_op(c, &inst->operands[0], loc);
            MInst vs = minst_make(MI_VA_START, 4u, loc);
            vs.op_count = 1u; vs.ops[0] = mop_vreg_use(ap_ptr);
            emit(c, vs);
            return;
        }

        case PIR_OP_VA_END:
            return;

        case PIR_OP_PTR_ADD: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg base = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                int32_t imm = (int32_t)eval_imm_u32(&inst->operands[1]);
                if (imm == 0) { emit_copy(c, dst, base, 4u, loc); return; }
                MInst ad = minst_make(MI_T2_ADDI, 4u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
                ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_imm((int64_t)imm);
                emit(c, ad);
            } else {
                MVReg off = mat_op(c, &inst->operands[1], loc);
                MInst ad = minst_make(MI_T2_ADD, 4u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
                ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(off);
                emit(c, ad);
            }
            return;
        }

        case PIR_OP_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = t2_val_width(inst->ty);
            if (t2_is_fp_type(inst->ty)) {
                if (inst->align > 0u && inst->align < 4u) {
                    /* ARMv7E-M vldr requires 4-byte alignment; use unaligned core ldr + vmov. */
                    MVReg lo = new_gp(c);
                    MInst l0 = minst_make(MI_T2_LD, 4u, loc);
                    l0.op_count = 3u; l0.ops[0] = mop_vreg_def(lo); l0.ops[1] = mop_vreg_use(ptr); l0.ops[2] = mop_imm(0);
                    emit(c, l0);
                    if (w == 8u) {
                        MVReg hi = new_gp(c);
                        MInst l1 = minst_make(MI_T2_LD, 4u, loc);
                        l1.op_count = 3u; l1.ops[0] = mop_vreg_def(hi); l1.ops[1] = mop_vreg_use(ptr); l1.ops[2] = mop_imm(4);
                        emit(c, l1);
                        MInst mv = minst_make(MI_T2_VMOV_G2D, 8u, loc);
                        mv.op_count = 3u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(lo); mv.ops[2] = mop_vreg_use(hi);
                        emit(c, mv);
                    } else {
                        MInst mv = minst_make(MI_T2_VMOV_G2S, 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(lo);
                        emit(c, mv);
                    }
                } else {
                    MInst ld = minst_make(MI_T2_VLD, w, loc);
                    ld.op_count = 3u;
                    ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
                    emit(c, ld);
                }
            } else {
                MInst ld = minst_make(MI_T2_LD, w, loc);
                ld.is_signed = false;
                ld.op_count  = 3u;
                ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
                emit(c, ld);
            }
            return;
        }

        case PIR_OP_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = t2_val_width(inst->ty);
            if (t2_is_fp_type(inst->ty)) {
                if (inst->align > 0u && inst->align < 4u) {
                    /* ARMv7E-M vstr requires 4-byte alignment; use vmov + unaligned core str. */
                    if (w == 8u) {
                        MVReg lo = new_gp(c), hi = new_gp(c);
                        MInst mv = minst_make(MI_T2_VMOV_D2G, 8u, loc);
                        mv.op_count = 3u; mv.ops[0] = mop_vreg_def(lo); mv.ops[1] = mop_vreg_def(hi); mv.ops[2] = mop_vreg_use(val);
                        emit(c, mv);
                        MInst s0 = minst_make(MI_T2_ST, 4u, loc);
                        s0.op_count = 3u; s0.ops[0] = mop_vreg_use(lo); s0.ops[1] = mop_vreg_use(ptr); s0.ops[2] = mop_imm(0);
                        emit(c, s0);
                        MInst s1 = minst_make(MI_T2_ST, 4u, loc);
                        s1.op_count = 3u; s1.ops[0] = mop_vreg_use(hi); s1.ops[1] = mop_vreg_use(ptr); s1.ops[2] = mop_imm(4);
                        emit(c, s1);
                    } else {
                        MVReg lo = new_gp(c);
                        MInst mv = minst_make(MI_T2_VMOV_S2G, 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(lo); mv.ops[1] = mop_vreg_use(val);
                        emit(c, mv);
                        MInst s0 = minst_make(MI_T2_ST, 4u, loc);
                        s0.op_count = 3u; s0.ops[0] = mop_vreg_use(lo); s0.ops[1] = mop_vreg_use(ptr); s0.ops[2] = mop_imm(0);
                        emit(c, s0);
                    }
                } else {
                    MInst st = minst_make(MI_T2_VST, w, loc);
                    st.op_count = 3u;
                    st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
                    emit(c, st);
                }
            } else {
                MInst st = minst_make(MI_T2_ST, w, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
                emit(c, st);
            }
            return;
        }

        case PIR_OP_ADD:
        case PIR_OP_SUB: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            bool trap_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
            bool trap_u = (inst->flags & PIR_FLAG_TRAP_U) != 0u;
            bool is_add = (inst->op == PIR_OP_ADD);

            if (!trap_s && !trap_u) {
                MVReg lhs = mat_op(c, &inst->operands[0], loc);
                if (is_add && inst->operands[1].kind == PIR_OPER_IMM_INT) {
                    MInst m = minst_make(MI_T2_ADDI, 4u, loc);
                    m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                    m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_imm((int64_t)(int32_t)eval_imm_u32(&inst->operands[1]));
                    emit(c, m);
                    return;
                }
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MInst m = minst_make(is_add ? MI_T2_ADD : MI_T2_SUB, 4u, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_vreg_use(rhs);
                emit(c, m);
                return;
            }

            /* Trapping overflow check */
            MVReg lhs = mat_ext(c, &inst->operands[0], w, trap_s, loc);
            MVReg rhs = mat_ext(c, &inst->operands[1], w, trap_s, loc);
            MVReg raw = new_gp(c);
            MInst m = minst_make(is_add ? MI_T2_ADD : MI_T2_SUB, 4u, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(raw);
            m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_vreg_use(rhs);
            emit(c, m);

            if (w < 4u) {
                MVReg ext_r = new_gp(c);
                emit_ext(c, ext_r, raw, w, trap_s, loc);
                emit_trap_if_cmp(c, MCOND_NE, raw, ext_r, loc);
            } else if (trap_u) {
                emit_trap_if_cmp(c, MCOND_CC, is_add ? raw : lhs, is_add ? lhs : rhs, loc);
            } else {
                /* Signed 32-bit overflow:
                 * add: ((raw ^ lhs) & (raw ^ rhs)) < 0
                 * sub: ((lhs ^ rhs) & (raw ^ lhs)) < 0 */
                MVReg x1 = new_gp(c), x2 = new_gp(c), ov = new_gp(c), zr = new_gp(c);
                MInst i1 = minst_make(MI_T2_XOR, 4u, loc);
                i1.op_count = 3u; i1.ops[0] = mop_vreg_def(x1);
                i1.ops[1] = mop_vreg_use(is_add ? raw : lhs); i1.ops[2] = mop_vreg_use(is_add ? lhs : rhs);
                emit(c, i1);
                MInst i2 = minst_make(MI_T2_XOR, 4u, loc);
                i2.op_count = 3u; i2.ops[0] = mop_vreg_def(x2);
                i2.ops[1] = mop_vreg_use(raw); i2.ops[2] = mop_vreg_use(is_add ? rhs : lhs);
                emit(c, i2);
                MInst i3 = minst_make(MI_T2_AND, 4u, loc);
                i3.op_count = 3u; i3.ops[0] = mop_vreg_def(ov); i3.ops[1] = mop_vreg_use(x1); i3.ops[2] = mop_vreg_use(x2);
                emit(c, i3);
                emit_li(c, zr, 0u, loc);
                emit_trap_if_cmp(c, MCOND_LT, ov, zr, loc);
            }
            emit_copy(c, dst, raw, 4u, loc);
            return;
        }

        case PIR_OP_MUL: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            bool trap_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
            bool trap_u = (inst->flags & PIR_FLAG_TRAP_U) != 0u;

            if (!trap_s && !trap_u) {
                MVReg lhs = mat_op(c, &inst->operands[0], loc);
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MInst ml = minst_make(MI_T2_MUL, 4u, loc);
                ml.op_count = 3u; ml.ops[0] = mop_vreg_def(dst);
                ml.ops[1] = mop_vreg_use(lhs); ml.ops[2] = mop_vreg_use(rhs);
                emit(c, ml);
                return;
            }

            MVReg lhs = mat_ext(c, &inst->operands[0], w, trap_s, loc);
            MVReg rhs = mat_ext(c, &inst->operands[1], w, trap_s, loc);
            if (w < 4u) {
                MVReg raw = new_gp(c);
                MInst ml = minst_make(MI_T2_MUL, 4u, loc);
                ml.op_count = 3u; ml.ops[0] = mop_vreg_def(raw);
                ml.ops[1] = mop_vreg_use(lhs); ml.ops[2] = mop_vreg_use(rhs);
                emit(c, ml);
                MVReg ext_r = new_gp(c);
                emit_ext(c, ext_r, raw, w, trap_s, loc);
                emit_trap_if_cmp(c, MCOND_NE, raw, ext_r, loc);
                emit_copy(c, dst, raw, 4u, loc);
            } else {
                MVReg lo = new_gp(c), hi = new_gp(c);
                MInst mw = minst_make(MI_T2_MUL_WIDE, 4u, loc);
                mw.is_signed = trap_s;
                mw.op_count  = 4u;
                mw.ops[0] = mop_vreg_def_ec(lo); mw.ops[1] = mop_vreg_def_ec(hi);
                mw.ops[2] = mop_vreg_use(lhs);   mw.ops[3] = mop_vreg_use(rhs);
                emit(c, mw);
                if (trap_u) {
                    MVReg zr = new_gp(c); emit_li(c, zr, 0u, loc);
                    emit_trap_if_cmp(c, MCOND_NE, hi, zr, loc);
                } else {
                    MVReg sgn = new_gp(c);
                    MInst sh = minst_make(MI_T2_ASRI, 4u, loc);
                    sh.op_count = 3u; sh.ops[0] = mop_vreg_def(sgn);
                    sh.ops[1] = mop_vreg_use(lo); sh.ops[2] = mop_imm(31);
                    emit(c, sh);
                    emit_trap_if_cmp(c, MCOND_NE, hi, sgn, loc);
                }
                emit_copy(c, dst, lo, 4u, loc);
            }
            return;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            bool is_s = (inst->op == PIR_OP_MUL_WIDE_S);
            uint8_t w = t2_val_width(inst->ty);
            MVReg dlo = c->val_vreg[inst->results[0]];
            MVReg dhi = c->val_vreg[inst->results[1]];
            MVReg lhs = mat_ext(c, &inst->operands[0], w, is_s, loc);
            MVReg rhs = mat_ext(c, &inst->operands[1], w, is_s, loc);
            if (w < 4u) {
                MVReg prod = new_gp(c);
                MInst ml = minst_make(MI_T2_MUL, 4u, loc);
                ml.op_count = 3u; ml.ops[0] = mop_vreg_def(prod);
                ml.ops[1] = mop_vreg_use(lhs); ml.ops[2] = mop_vreg_use(rhs);
                emit(c, ml);
                emit_copy(c, dlo, prod, 4u, loc);
                MInst sh = minst_make(is_s ? MI_T2_ASRI : MI_T2_LSRI, 4u, loc);
                sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dhi);
                sh.ops[1] = mop_vreg_use(prod); sh.ops[2] = mop_imm((int64_t)(w * 8u));
                emit(c, sh);
            } else {
                MInst mw = minst_make(MI_T2_MUL_WIDE, 4u, loc);
                mw.is_signed = is_s;
                mw.op_count  = 4u;
                mw.ops[0] = mop_vreg_def_ec(dlo); mw.ops[1] = mop_vreg_def_ec(dhi);
                mw.ops[2] = mop_vreg_use(lhs);    mw.ops[3] = mop_vreg_use(rhs);
                emit(c, mw);
            }
            return;
        }

        case PIR_OP_DIV_S:
        case PIR_OP_DIV_U:
        case PIR_OP_REM_S:
        case PIR_OP_REM_U:
            isel_div_rem(c, inst, loc);
            return;

        case PIR_OP_AND:
        case PIR_OP_OR:
        case PIR_OP_XOR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                MOpcode mop = (inst->op == PIR_OP_AND) ? MI_T2_ANDI
                            : (inst->op == PIR_OP_OR)  ? MI_T2_ORI : MI_T2_XORI;
                MInst m = minst_make(mop, 4u, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_imm((int64_t)(int32_t)eval_imm_u32(&inst->operands[1]));
                emit(c, m);
            } else {
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MOpcode mop = (inst->op == PIR_OP_AND) ? MI_T2_AND
                            : (inst->op == PIR_OP_OR)  ? MI_T2_OR : MI_T2_XOR;
                MInst m = minst_make(mop, 4u, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_vreg_use(rhs);
                emit(c, m);
            }
            return;
        }

        case PIR_OP_SHL:
        case PIR_OP_SHR:
        case PIR_OP_SAR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            uint32_t bits = (uint32_t)w * 8u;
            bool is_sar = (inst->op == PIR_OP_SAR);
            bool is_shr = (inst->op == PIR_OP_SHR || is_sar);
            MVReg lhs = is_shr ? mat_ext(c, &inst->operands[0], w, is_sar, loc)
                               : mat_op(c, &inst->operands[0], loc);

            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint32_t sh = eval_imm_u32(&inst->operands[1]);
                if (inst->flags & PIR_FLAG_TRAP) {
                    if (sh >= bits) { emit(c, minst_make(MI_T2_TRAP, 4u, loc)); return; }
                } else {
                    sh &= (bits - 1u);
                }
                if (sh == 0u) { emit_copy(c, dst, lhs, 4u, loc); return; }
                MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_T2_SHLI
                            : is_sar ? MI_T2_ASRI : MI_T2_LSRI;
                MInst m = minst_make(mop, 4u, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_imm((int64_t)sh);
                emit(c, m);
            } else {
                MVReg rhs = mat_ext(c, &inst->operands[1], w, false, loc);
                if (inst->flags & PIR_FLAG_TRAP) {
                    MVReg lim = new_gp(c); emit_li(c, lim, bits, loc);
                    emit_trap_if_cmp(c, MCOND_CS, rhs, lim, loc);
                }
                MVReg shv = new_gp(c);
                MInst mk = minst_make(MI_T2_ANDI, 4u, loc);
                mk.op_count = 3u; mk.ops[0] = mop_vreg_def(shv);
                mk.ops[1] = mop_vreg_use(rhs); mk.ops[2] = mop_imm((int64_t)(bits - 1u));
                emit(c, mk);

                MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_T2_SHL
                            : is_sar ? MI_T2_ASR : MI_T2_LSR;
                MInst m = minst_make(mop, 4u, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst);
                m.ops[1] = mop_vreg_use(lhs); m.ops[2] = mop_vreg_use(shv);
                emit(c, m);
            }
            return;
        }

        case PIR_OP_CLZ: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg src = mat_ext(c, &inst->operands[0], w, false, loc);
            MVReg raw = (w < 4u) ? new_gp(c) : dst;
            MInst b = minst_make(MI_T2_BITOP, 4u, loc);
            b.aux_id = 0u; b.op_count = 2u; b.ops[0] = mop_vreg_def(raw); b.ops[1] = mop_vreg_use(src);
            emit(c, b);
            if (w < 4u) {
                MInst sb = minst_make(MI_T2_ADDI, 4u, loc);
                sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst);
                sb.ops[1] = mop_vreg_use(raw); sb.ops[2] = mop_imm(-(int64_t)(32u - w * 8u));
                emit(c, sb);
            }
            return;
        }

        case PIR_OP_CTZ: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg src = mat_ext(c, &inst->operands[0], w, false, loc);
            if (w < 4u) {
                MVReg sent = new_gp(c);
                MInst o = minst_make(MI_T2_ORI, 4u, loc);
                o.op_count = 3u; o.ops[0] = mop_vreg_def(sent);
                o.ops[1] = mop_vreg_use(src); o.ops[2] = mop_imm((int64_t)(1u << (w * 8u)));
                emit(c, o);
                src = sent;
            }
            MInst b = minst_make(MI_T2_BITOP, 4u, loc);
            b.aux_id = 1u; b.op_count = 2u; b.ops[0] = mop_vreg_def(dst); b.ops[1] = mop_vreg_use(src);
            emit(c, b);
            return;
        }

        case PIR_OP_BSWAP: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (w == 1u) { emit_copy(c, dst, src, 4u, loc); return; }
            MInst b = minst_make(MI_T2_BITOP, w, loc);
            b.aux_id = 2u; b.op_count = 2u; b.ops[0] = mop_vreg_def(dst); b.ops[1] = mop_vreg_use(src);
            emit(c, b);
            return;
        }

        case PIR_OP_POPCNT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg src = mat_ext(c, &inst->operands[0], w, false, loc);
            MInst b = minst_make(MI_T2_BITOP, 4u, loc);
            b.aux_id = 4u; b.op_count = 2u; b.ops[0] = mop_vreg_def_ec(dst); b.ops[1] = mop_vreg_use(src);
            emit(c, b);
            return;
        }

        case PIR_OP_TRUNC:
        case PIR_OP_PTRTOINT:
        case PIR_OP_INTTOPTR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            emit_copy(c, dst, src, 4u, loc);
            return;
        }

        case PIR_OP_ZEXT:
        case PIR_OP_SEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t sw = t2_val_width(inst->operands[0].type);
            MVReg src = mat_op(c, &inst->operands[0], loc);
            emit_ext(c, dst, src, sw, inst->op == PIR_OP_SEXT, loc);
            return;
        }

        case PIR_OP_BITCAST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            bool dst_fp = t2_is_fp_type(inst->ty);
            bool src_fp = t2_is_fp_type(inst->operands[0].type);
            if (!dst_fp && !src_fp) {
                emit_copy(c, dst, src, 4u, loc);
            } else if (dst_fp && src_fp) {
                emit_copy(c, dst, src, t2_val_width(inst->ty), loc);
            } else if (dst_fp && !src_fp) {
                /* i32 -> f32 */
                MInst mv = minst_make(MI_T2_VMOV_G2S, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            } else {
                /* f32 -> i32 */
                MInst mv = minst_make(MI_T2_VMOV_S2G, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            }
            return;
        }

        case PIR_OP_FADD:
        case PIR_OP_FSUB:
        case PIR_OP_FMUL:
        case PIR_OP_FDIV: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MOpcode mop = (inst->op == PIR_OP_FADD) ? MI_T2_FADD
                        : (inst->op == PIR_OP_FSUB) ? MI_T2_FSUB
                        : (inst->op == PIR_OP_FMUL) ? MI_T2_FMUL : MI_T2_FDIV;
            MInst f = minst_make(mop, w, loc);
            f.op_count = 3u; f.ops[0] = mop_vreg_def(dst);
            f.ops[1] = mop_vreg_use(lhs); f.ops[2] = mop_vreg_use(rhs);
            emit(c, f);
            return;
        }

        case PIR_OP_FNEG: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst f = minst_make(MI_T2_FNEG, w, loc);
            f.op_count = 2u; f.ops[0] = mop_vreg_def(dst); f.ops[1] = mop_vreg_use(src);
            emit(c, f);
            return;
        }

        case PIR_OP_FPEXT:
        case PIR_OP_FPTRUNC: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst f = minst_make(MI_T2_FCVT_F2F, t2_val_width(inst->ty), loc);
            f.op_count = 2u; f.ops[0] = mop_vreg_def(dst); f.ops[1] = mop_vreg_use(src);
            emit(c, f);
            return;
        }

        case PIR_OP_SITOFP:
        case PIR_OP_UITOFP: {
            bool is_s = (inst->op == PIR_OP_SITOFP);
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t sw = t2_val_width(inst->operands[0].type);
            MVReg src = mat_ext(c, &inst->operands[0], sw, is_s, loc);
            MInst f = minst_make(MI_T2_FCVT_I2F, t2_val_width(inst->ty), loc);
            f.is_signed = is_s;
            f.op_count  = 2u; f.ops[0] = mop_vreg_def(dst); f.ops[1] = mop_vreg_use(src);
            emit(c, f);
            return;
        }

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI: {
            bool is_s = (inst->op == PIR_OP_FPTOSI);
            bool is_sat = (inst->flags & PIR_FLAG_SAT) != 0u;
            uint8_t dw = t2_val_width(inst->ty);
            uint8_t fw = t2_val_width(inst->operands[0].type);
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);

            if (!is_sat) {
                /* Trap on NaN or out-of-range! Check src == src (ordered). */
                MVReg ord = new_gp(c);
                MInst fc = minst_make(MI_T2_FCMP_SET, fw, loc);
                fc.aux_id = 0u;
                fc.op_count = 4u; fc.ops[0] = mop_vreg_def(ord);
                fc.ops[1] = mop_vreg_use(src); fc.ops[2] = mop_vreg_use(src);
                fc.ops[3] = mop_cond(MCOND_NE); /* unordered or ne -> trap! */
                fc.aux_id = 1u; /* unordered_true */
                emit(c, fc);
                emit_trap_if_nz(c, ord, loc);

                /* Range check using exact float bounds:
                 * i8:  [-128, 128)   u8:  [0, 256)
                 * i16: [-32768, 32768) u16: [0, 65536)
                 * i32: [-2147483648, 2147483648) u32: [0, 4294967296) */
                double d_lo = is_s ? (dw == 1u ? -128.0 : dw == 2u ? -32768.0 : -2147483648.0) : 0.0;
                double d_hi = is_s ? (dw == 1u ?  128.0 : dw == 2u ?  32768.0 :  2147483648.0)
                                   : (dw == 1u ?  256.0 : dw == 2u ?  65536.0 :  4294967296.0);
                uint64_t lo_bits = 0u, hi_bits = 0u;
                if (fw == 4u) {
                    float fl = (float)d_lo, fh = (float)d_hi;
                    uint32_t bl = 0u, bh = 0u;
                    memcpy(&bl, &fl, 4u); memcpy(&bh, &fh, 4u);
                    lo_bits = bl; hi_bits = bh;
                } else {
                    memcpy(&lo_bits, &d_lo, 8u); memcpy(&hi_bits, &d_hi, 8u);
                }
                MVReg v_lo = new_fp(c, fw), v_hi = new_fp(c, fw);
                emit_load_imm_fp(c, v_lo, lo_bits, fw, loc);
                emit_load_imm_fp(c, v_hi, hi_bits, fw, loc);

                MVReg bad_lo = new_gp(c), bad_hi = new_gp(c);
                MInst cl = minst_make(MI_T2_FCMP_SET, fw, loc);
                cl.op_count = 4u; cl.ops[0] = mop_vreg_def(bad_lo);
                cl.ops[1] = mop_vreg_use(src); cl.ops[2] = mop_vreg_use(v_lo); cl.ops[3] = mop_cond(MCOND_MI);
                emit(c, cl);
                emit_trap_if_nz(c, bad_lo, loc);

                MInst ch = minst_make(MI_T2_FCMP_SET, fw, loc);
                ch.op_count = 4u; ch.ops[0] = mop_vreg_def(bad_hi);
                ch.ops[1] = mop_vreg_use(src); ch.ops[2] = mop_vreg_use(v_hi); ch.ops[3] = mop_cond(MCOND_GE);
                emit(c, ch);
                emit_trap_if_nz(c, bad_hi, loc);
            }

            /* VFP vcvt.{s32,u32}.{f32,f64} natively saturates to 32-bit range & NaN->0! */
            MVReg raw = (dw < 4u && is_sat) ? new_gp(c) : dst;
            MInst cv = minst_make(MI_T2_FCVT_F2I, fw, loc);
            cv.is_signed = is_s;
            cv.aux_id    = is_sat ? 1u : 0u;
            cv.op_count  = 2u; cv.ops[0] = mop_vreg_def(raw); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);

            if (dw < 4u && is_sat) {
                uint32_t mn = is_s ? (dw == 1u ? 0xFFFFFF80u : 0xFFFF8000u) : 0u;
                uint32_t mx = is_s ? (dw == 1u ? 127u : 32767u) : (dw == 1u ? 255u : 65535u);
                MVReg rmn = new_gp(c), rmx = new_gp(c);
                emit_li(c, rmn, mn, loc); emit_li(c, rmx, mx, loc);
                MVReg c_lo = new_gp(c), t1 = new_gp(c), c_hi = new_gp(c);

                MInst cm1 = minst_make(MI_T2_CMP_SET, 4u, loc);
                cm1.op_count = 4u; cm1.ops[0] = mop_vreg_def(c_lo);
                cm1.ops[1] = mop_vreg_use(raw); cm1.ops[2] = mop_vreg_use(rmn);
                cm1.ops[3] = mop_cond(is_s ? MCOND_LT : MCOND_CC);
                emit(c, cm1);
                MInst s1 = minst_make(MI_T2_SELECT, 4u, loc);
                s1.op_count = 4u; s1.ops[0] = mop_vreg_def(t1);
                s1.ops[1] = mop_vreg_use(c_lo); s1.ops[2] = mop_vreg_use(rmn); s1.ops[3] = mop_vreg_use(raw);
                emit(c, s1);

                MInst cm2 = minst_make(MI_T2_CMP_SET, 4u, loc);
                cm2.op_count = 4u; cm2.ops[0] = mop_vreg_def(c_hi);
                cm2.ops[1] = mop_vreg_use(t1); cm2.ops[2] = mop_vreg_use(rmx);
                cm2.ops[3] = mop_cond(is_s ? MCOND_GT : MCOND_HI);
                emit(c, cm2);
                MInst s2 = minst_make(MI_T2_SELECT, 4u, loc);
                s2.op_count = 4u; s2.ops[0] = mop_vreg_def(dst);
                s2.ops[1] = mop_vreg_use(c_hi); s2.ops[2] = mop_vreg_use(rmx); s2.ops[3] = mop_vreg_use(t1);
                emit(c, s2);
            }
            return;
        }

        case PIR_OP_CMP_EQ: case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LE_S: case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GE_S:
        case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_U: case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_U: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t w = t2_val_width(inst->ty);
            bool is_s = (inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_LE_S ||
                         inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_GE_S);
            MVReg lhs = mat_ext(c, &inst->operands[0], w, is_s, loc);
            MVReg rhs = mat_ext(c, &inst->operands[1], w, is_s, loc);
            MInst cm = minst_make(MI_T2_CMP_SET, 4u, loc);
            cm.op_count = 4u;
            cm.ops[0] = mop_vreg_def(dst);
            cm.ops[1] = mop_vreg_use(lhs);
            cm.ops[2] = mop_vreg_use(rhs);
            cm.ops[3] = mop_cond(pir_cmp_to_mcond(inst->op));
            emit(c, cm);
            return;
        }

        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE:
        case PIR_OP_FCMP_OLT: case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE:
        case PIR_OP_FCMP_ULT: case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint8_t fw = t2_val_width(inst->ty);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MCond cc = MCOND_EQ;
            uint32_t unord_true = 0u;
            if (inst->op == PIR_OP_FCMP_OEQ)      { cc = MCOND_EQ; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_OLT) { cc = MCOND_MI; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_OLE) { cc = MCOND_LS; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_OGT) { cc = MCOND_GT; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_OGE) { cc = MCOND_GE; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_UNE) { cc = MCOND_NE; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_ULT) { cc = MCOND_LT; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_ULE) { cc = MCOND_LE; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_UGT) { cc = MCOND_HI; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_UGE) { cc = MCOND_PL; unord_true = 0u; }
            else if (inst->op == PIR_OP_FCMP_ONE) { cc = MCOND_NE; unord_true = 2u; }
            else if (inst->op == PIR_OP_FCMP_UEQ) { cc = MCOND_EQ; unord_true = 1u; }
            MInst fc = minst_make(MI_T2_FCMP_SET, fw, loc);
            fc.aux_id   = unord_true;
            fc.op_count = 4u;
            fc.ops[0] = mop_vreg_def(dst);
            fc.ops[1] = mop_vreg_use(lhs);
            fc.ops[2] = mop_vreg_use(rhs);
            fc.ops[3] = mop_cond(cc);
            emit(c, fc);
            return;
        }

        case PIR_OP_SELECT: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg cond = mat_ext(c, &inst->operands[0], 1u, false, loc);
            MVReg tv   = mat_op(c, &inst->operands[1], loc);
            MVReg fv   = mat_op(c, &inst->operands[2], loc);
            if (t2_is_fp_type(inst->ty)) {
                MInst sl = minst_make(MI_T2_FSELECT, t2_val_width(inst->ty), loc);
                sl.op_count = 4u; sl.ops[0] = mop_vreg_def(dst);
                sl.ops[1] = mop_vreg_use(cond); sl.ops[2] = mop_vreg_use(tv); sl.ops[3] = mop_vreg_use(fv);
                emit(c, sl);
            } else {
                MInst sl = minst_make(MI_T2_SELECT, 4u, loc);
                sl.op_count = 4u; sl.ops[0] = mop_vreg_def(dst);
                sl.ops[1] = mop_vreg_use(cond); sl.ops[2] = mop_vreg_use(tv); sl.ops[3] = mop_vreg_use(fv);
                emit(c, sl);
            }
            return;
        }

        case PIR_OP_FENCE:
            emit(c, minst_make(MI_T2_DMB, 4u, loc));
            return;

        case PIR_OP_ATOMIC_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MInst al = minst_make(MI_T2_ATOMIC_LD, t2_val_width(inst->ty), loc);
            al.op_count = 2u; al.ops[0] = mop_vreg_def(dst); al.ops[1] = mop_vreg_use(ptr);
            emit(c, al);
            return;
        }

        case PIR_OP_ATOMIC_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            MInst as = minst_make(MI_T2_ATOMIC_ST, t2_val_width(inst->ty), loc);
            as.op_count = 2u; as.ops[0] = mop_vreg_use(val); as.ops[1] = mop_vreg_use(ptr);
            emit(c, as);
            return;
        }

        case PIR_OP_ATOMIC_RMW: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);

            /* Pin operands to r0..r2 via MI_PCOPY so r12/r11 scratches never collide with spills. */
            MVReg p_dst = new_gp(c); c->mf->vregs.data[p_dst].fixed_preg = T2_R0;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = T2_R1;
            MVReg p_val = new_gp(c); c->mf->vregs.data[p_val].fixed_preg = T2_R2;
            MPCopyEntry ents[2];
            ents[0].dst = p_ptr; ents[0].src_is_imm = false; ents[0].src = ptr; ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 4u;
            ents[1].dst = p_val; ents[1].src_is_imm = false; ents[1].src = val; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 4u;
            uint32_t pid = mfunc_add_pcopy(c->mf, ents, 2u);
            MInst pc = minst_make(MI_PCOPY, 4u, loc); pc.aux_id = pid; emit(c, pc);

            MInst ar = minst_make(MI_T2_ATOMIC_RMW, t2_val_width(inst->ty), loc);
            ar.aux_id   = (uint32_t)inst->rmw_op;
            ar.op_count = 3u;
            ar.ops[0] = mop_vreg_def_ec(p_dst);
            ar.ops[1] = mop_vreg_use(p_ptr);
            ar.ops[2] = mop_vreg_use(p_val);
            emit(c, ar);
            emit_copy(c, dst, p_dst, 4u, loc);
            return;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            MVReg d_old = c->val_vreg[inst->results[0]];
            MVReg d_ok  = c->val_vreg[inst->results[1]];
            uint8_t w = t2_val_width(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg exp = mat_ext(c, &inst->operands[1], w, false, loc);
            MVReg des = mat_op(c, &inst->operands[2], loc);

            /* Pin operands to r0..r4 via MI_PCOPY so r12 scratch never collides with spills. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = T2_R0;
            MVReg p_ok  = new_gp(c); c->mf->vregs.data[p_ok].fixed_preg  = T2_R1;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = T2_R2;
            MVReg p_exp = new_gp(c); c->mf->vregs.data[p_exp].fixed_preg = T2_R3;
            MVReg p_des = new_gp(c); c->mf->vregs.data[p_des].fixed_preg = T2_R4;
            MPCopyEntry ents[3];
            ents[0].dst = p_ptr; ents[0].src_is_imm = false; ents[0].src = ptr; ents[0].rclass = MREG_CLASS_GP; ents[0].byte_size = 4u;
            ents[1].dst = p_exp; ents[1].src_is_imm = false; ents[1].src = exp; ents[1].rclass = MREG_CLASS_GP; ents[1].byte_size = 4u;
            ents[2].dst = p_des; ents[2].src_is_imm = false; ents[2].src = des; ents[2].rclass = MREG_CLASS_GP; ents[2].byte_size = 4u;
            uint32_t pid = mfunc_add_pcopy(c->mf, ents, 3u);
            MInst pc = minst_make(MI_PCOPY, 4u, loc); pc.aux_id = pid; emit(c, pc);

            MInst ac = minst_make(MI_T2_ATOMIC_CAS, w, loc);
            ac.op_count = 5u;
            ac.ops[0] = mop_vreg_def_ec(p_old);
            ac.ops[1] = mop_vreg_def_ec(p_ok);
            ac.ops[2] = mop_vreg_use(p_ptr);
            ac.ops[3] = mop_vreg_use(p_exp);
            ac.ops[4] = mop_vreg_use(p_des);
            emit(c, ac);
            emit_copy(c, d_old, p_old, 4u, loc);
            emit_copy(c, d_ok,  p_ok,  4u, loc);
            return;
        }

        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
            isel_call(c, inst, loc);
            return;

        case PIR_OP_JMP: {
            emit_branch_pcopy(c, &inst->targets[0], loc);
            uint32_t dst_mb = c->blk_map[inst->targets[0].target];
            MInst j = minst_make(MI_T2_B, 4u, loc);
            j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
            emit(c, j);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_mb);
            return;
        }

        case PIR_OP_BR: {
            MVReg cond = mat_ext(c, &inst->operands[0], 1u, false, loc);
            uint32_t t_mb = resolve_edge_target(c, &inst->targets[0], loc);
            uint32_t f_mb = resolve_edge_target(c, &inst->targets[1], loc);

            MInst br = minst_make(MI_T2_BR_NZ, 4u, loc);
            br.op_count = 2u; br.ops[0] = mop_vreg_use(cond); br.ops[1] = mop_block(t_mb);
            emit(c, br);

            MInst j = minst_make(MI_T2_B, 4u, loc);
            j.op_count = 1u; j.ops[0] = mop_block(f_mb);
            emit(c, j);

            mfunc_add_edge(c->mf, c->cur_mblk, t_mb);
            mfunc_add_edge(c->mf, c->cur_mblk, f_mb);
            return;
        }

        case PIR_OP_SWITCH: {
            uint8_t w = t2_val_width(inst->ty);
            MVReg val = mat_ext(c, &inst->operands[0], w, true, loc);
            uint32_t nc = inst->switch_case_count;
            uint32_t def_mb = resolve_edge_target(c, &inst->targets[0], loc);

            int64_t min_v = INT64_MAX, max_v = INT64_MIN;
            for (uint32_t k = 0u; k < nc; ++k) {
                int64_t raw_cv = inst->switch_cases[k].is_negative
                                 ? -(int64_t)inst->switch_cases[k].case_lo
                                 : (int64_t)inst->switch_cases[k].case_lo;
                int64_t cv = (w == 1u) ? (int64_t)(int8_t)raw_cv
                           : (w == 2u) ? (int64_t)(int16_t)raw_cv
                                       : (int64_t)(int32_t)raw_cv;
                if (cv < min_v) min_v = cv;
                if (cv > max_v) max_v = cv;
            }
            uint64_t span = (nc > 0u) ? ((uint64_t)(max_v - min_v) + 1u) : 0u;
            bool use_tbl = (nc >= 4u && span <= 256u && span <= (uint64_t)nc * 4u);

            if (use_tbl) {
                uint32_t *tbl = (uint32_t *)malloc(span * sizeof(uint32_t));
                if (!tbl) abort();
                for (uint64_t s = 0u; s < span; ++s) tbl[s] = def_mb;
                for (uint32_t k = 0u; k < nc; ++k) {
                    int64_t raw_cv = inst->switch_cases[k].is_negative
                                     ? -(int64_t)inst->switch_cases[k].case_lo
                                     : (int64_t)inst->switch_cases[k].case_lo;
                    int64_t cv = (w == 1u) ? (int64_t)(int8_t)raw_cv
                               : (w == 2u) ? (int64_t)(int16_t)raw_cv
                                           : (int64_t)(int32_t)raw_cv;
                    uint32_t tgt = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                    tbl[(uint64_t)(cv - min_v)] = tgt;
                }
                uint32_t jtid = mfunc_add_jtable(c->mf, min_v, (uint32_t)span, tbl, def_mb);
                MVReg idx = val;
                if (min_v != 0) {
                    idx = new_gp(c);
                    MInst sb = minst_make(MI_T2_ADDI, 4u, loc);
                    sb.op_count = 3u; sb.ops[0] = mop_vreg_def(idx);
                    sb.ops[1] = mop_vreg_use(val); sb.ops[2] = mop_imm(-min_v);
                    emit(c, sb);
                }
                MInst st = minst_make(MI_T2_SWITCH_TBL, 4u, loc);
                st.op_count = 2u; st.ops[0] = mop_vreg_use(idx); st.ops[1] = mop_jtable(jtid);
                emit(c, st);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mb);
                for (uint64_t s = 0u; s < span; ++s) {
                    if (tbl[s] != def_mb) mfunc_add_edge(c->mf, c->cur_mblk, tbl[s]);
                }
                free(tbl);
            } else {
                for (uint32_t k = 0u; k < nc; ++k) {
                    int64_t raw_cv = inst->switch_cases[k].is_negative
                                     ? -(int64_t)inst->switch_cases[k].case_lo
                                     : (int64_t)inst->switch_cases[k].case_lo;
                    uint32_t cv = (w == 1u) ? (uint32_t)(int32_t)(int8_t)raw_cv
                                : (w == 2u) ? (uint32_t)(int32_t)(int16_t)raw_cv
                                            : (uint32_t)raw_cv;
                    uint32_t tgt = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                    MVReg kv = new_gp(c); emit_li(c, kv, cv, loc);
                    MInst br = minst_make(MI_T2_BC, 4u, loc);
                    br.op_count = 4u;
                    br.ops[0] = mop_vreg_use(val); br.ops[1] = mop_vreg_use(kv);
                    br.ops[2] = mop_cond(MCOND_EQ); br.ops[3] = mop_block(tgt);
                    emit(c, br);
                    mfunc_add_edge(c->mf, c->cur_mblk, tgt);
                }
                MInst j = minst_make(MI_T2_B, 4u, loc);
                j.op_count = 1u; j.ops[0] = mop_block(def_mb);
                emit(c, j);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mb);
            }
            return;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && c->multi_ret_vreg != MVREG_NONE) {
                uint32_t rc = c->fn->cabi_ret.reg_count;
                if (rc > 4u) rc = 4u;
                bool ret_fp = (rc > 0u && t2_is_fp_type(c->fn->cabi_ret.regs[0]));
                if (ret_fp && c->fn->cabi_ret.regs[0].u.scalar == PIR_SCALAR_F32) {
                    MVReg fvs[4];
                    for (uint32_t k = 0u; k < rc; ++k) {
                        fvs[k] = new_fp(c, 4u);
                        MInst ld = minst_make(MI_T2_VLD, 4u, loc);
                        ld.op_count = 3u;
                        ld.ops[0] = mop_vreg_def(fvs[k]);
                        ld.ops[1] = mop_vreg_use(c->multi_ret_vreg);
                        ld.ops[2] = mop_imm((int64_t)(k * 4u));
                        emit(c, ld);
                    }
                    uint32_t nd = (rc + 1u) / 2u;
                    MVReg pks[2] = { MVREG_NONE, MVREG_NONE };
                    for (uint32_t d = 0u; d < nd; ++d) {
                        if (d * 2u + 1u < rc) {
                            pks[d] = new_fp(c, 8u);
                            MInst pm = minst_make(MI_T2_PACK_S2, 8u, loc);
                            pm.op_count = 3u; pm.ops[0] = mop_vreg_def_ec(pks[d]);
                            pm.ops[1] = mop_vreg_use(fvs[d * 2u]); pm.ops[2] = mop_vreg_use(fvs[d * 2u + 1u]);
                            emit(c, pm);
                        } else {
                            pks[d] = fvs[d * 2u];
                        }
                    }
                    for (uint32_t d = 0u; d < nd; ++d) {
                        uint8_t w = (d * 2u + 1u < rc) ? 8u : 4u;
                        MInst cp = minst_make(MI_COPY, w, loc);
                        cp.op_count = 2u; cp.ops[0] = mop_preg_def(T2_D(d)); cp.ops[1] = mop_vreg_use(pks[d]);
                        emit(c, cp);
                    }
                } else if (ret_fp) {
                    MVReg fvs[4];
                    for (uint32_t k = 0u; k < rc; ++k) {
                        fvs[k] = new_fp(c, 8u);
                        MInst ld = minst_make(MI_T2_VLD, 8u, loc);
                        ld.op_count = 3u;
                        ld.ops[0] = mop_vreg_def(fvs[k]);
                        ld.ops[1] = mop_vreg_use(c->multi_ret_vreg);
                        ld.ops[2] = mop_imm((int64_t)(k * 8u));
                        emit(c, ld);
                    }
                    for (uint32_t k = 0u; k < rc; ++k) {
                        MInst cp = minst_make(MI_COPY, 8u, loc);
                        cp.op_count = 2u; cp.ops[0] = mop_preg_def(T2_D(k)); cp.ops[1] = mop_vreg_use(fvs[k]);
                        emit(c, cp);
                    }
                } else {
                    MVReg gvs[4];
                    for (uint32_t k = 0u; k < rc; ++k) {
                        gvs[k] = new_gp(c);
                        MInst ld = minst_make(MI_T2_LD, 4u, loc);
                        ld.op_count = 3u;
                        ld.ops[0] = mop_vreg_def(gvs[k]);
                        ld.ops[1] = mop_vreg_use(c->multi_ret_vreg);
                        ld.ops[2] = mop_imm((int64_t)(k * 4u));
                        emit(c, ld);
                    }
                    for (uint32_t k = 0u; k < rc; ++k) {
                        MInst cp = minst_make(MI_COPY, 4u, loc);
                        cp.op_count = 2u; cp.ops[0] = mop_preg_def(T2_GP(k)); cp.ops[1] = mop_vreg_use(gvs[k]);
                        emit(c, cp);
                    }
                }
            } else if (inst->operand_count >= 1u) {
                uint8_t rw = t2_val_width(c->fn->ret_type);
                if (t2_is_fp_type(c->fn->ret_type)) {
                    MVReg rv = mat_op(c, &inst->operands[0], loc);
                    if (c->fn->is_variadic || !c->target->caps.has_hw_fp) {
                        if (rw == 8u) {
                            MVReg lo = new_gp(c), hi = new_gp(c);
                            MInst mv = minst_make(MI_T2_VMOV_D2G, 8u, loc);
                            mv.op_count = 3u; mv.ops[0] = mop_vreg_def(lo); mv.ops[1] = mop_vreg_def(hi); mv.ops[2] = mop_vreg_use(rv);
                            emit(c, mv);
                            MInst c0 = minst_make(MI_COPY, 4u, loc);
                            c0.op_count = 2u; c0.ops[0] = mop_preg_def(T2_R0); c0.ops[1] = mop_vreg_use(lo);
                            emit(c, c0);
                            MInst c1 = minst_make(MI_COPY, 4u, loc);
                            c1.op_count = 2u; c1.ops[0] = mop_preg_def(T2_R1); c1.ops[1] = mop_vreg_use(hi);
                            emit(c, c1);
                        } else {
                            MVReg g = new_gp(c);
                            MInst mv = minst_make(MI_T2_VMOV_S2G, 4u, loc);
                            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(g); mv.ops[1] = mop_vreg_use(rv);
                            emit(c, mv);
                            MInst c0 = minst_make(MI_COPY, 4u, loc);
                            c0.op_count = 2u; c0.ops[0] = mop_preg_def(T2_R0); c0.ops[1] = mop_vreg_use(g);
                            emit(c, c0);
                        }
                    } else {
                        MInst cp = minst_make(MI_COPY, rw, loc);
                        cp.op_count = 2u; cp.ops[0] = mop_preg_def(T2_D(0)); cp.ops[1] = mop_vreg_use(rv);
                        emit(c, cp);
                    }
                } else {
                    bool sext = (c->fn->attrs & PIR_FATTR_RET_SIGNEXT) != 0u;
                    MVReg rv = mat_ext(c, &inst->operands[0], rw, sext, loc);
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_preg_def(T2_R0); cp.ops[1] = mop_vreg_use(rv);
                    emit(c, cp);
                }
            }
            emit(c, minst_make(MI_T2_RET, 4u, loc));
            return;
        }

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE:
            emit(c, minst_make(MI_T2_TRAP, 4u, loc));
            return;

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_VA_COPY: case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
            pir_diag_error(c->diag, inst->src_line, 0u,
                           "Thumb-2 ISel: unsupported LIR opcode %u", (unsigned)inst->op);
            c->ok = false;
            return;
    }
}

/* =========================================================================
 * Entry Function Driver
 * ========================================================================= */

bool pir_thumb2_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                          const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);
    out_mf->is_variadic = fn->is_variadic;

    T2ISelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod            = mod;
    ctx.fn             = fn;
    ctx.target         = target;
    ctx.mf             = out_mf;
    ctx.diag           = diag;
    ctx.ok             = true;
    ctx.multi_ret_vreg = MVREG_NONE;

    uint32_t nv = fn->values.len;
    uint32_t ni = fn->insts.len;
    uint32_t nb = fn->blocks.len;

    ctx.val_vreg    = nv ? (MVReg *)malloc(nv * sizeof(MVReg)) : NULL;
    ctx.alloca_slot = ni ? (uint32_t *)malloc(ni * sizeof(uint32_t)) : NULL;
    ctx.blk_map     = nb ? (uint32_t *)malloc(nb * sizeof(uint32_t)) : NULL;
    if ((nv && !ctx.val_vreg) || (ni && !ctx.alloca_slot) || (nb && !ctx.blk_map)) abort();

    for (uint32_t v = 0u; v < nv; ++v) {
        PirType ty = fn->values.data[v].type;
        if (t2_is_fp_type(ty)) {
            ctx.val_vreg[v] = mfunc_new_vreg(out_mf, MREG_CLASS_FP, t2_val_width(ty));
        } else {
            ctx.val_vreg[v] = mfunc_new_vreg(out_mf, MREG_CLASS_GP, 4u);
        }
    }

    for (uint32_t i = 0u; i < ni; ++i) {
        const PirInst *ins = &fn->insts.data[i];
        ctx.alloca_slot[i] = UINT32_MAX;
        if (ins->op == PIR_OP_ALLOCA_PTR) {
            uint64_t sz = ins->imm_u64 > 0u ? ins->imm_u64 : 1u;
            uint32_t al = ins->align > 0u ? ins->align : 4u;
            ctx.alloca_slot[i] = mfunc_add_slot(out_mf, sz, al, false);
        }
    }

    for (uint32_t b = 0u; b < nb; ++b) {
        ctx.blk_map[b] = mfunc_new_block(out_mf, fn->blocks.data[b].label);
    }

    ctx.cur_mblk = ctx.blk_map[0];

    /* Emit entry parameter materialization. */
    uint32_t first_p = 0u;
    if (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && fn->param_count >= 1u) {
        uint64_t rsz = fn->cabi_ret.agg_size > 0u ? fn->cabi_ret.agg_size : 16u;
        uint32_t ral = fn->cabi_ret.agg_align >= 4u ? fn->cabi_ret.agg_align : 4u;
        uint32_t rslot = mfunc_add_slot(out_mf, rsz, ral, false);
        ctx.multi_ret_vreg = ctx.val_vreg[fn->params[0].val_id];
        MInst fa = minst_make(MI_FRAME_ADDR, 4u, 0u);
        fa.op_count = 3u; fa.ops[0] = mop_vreg_def(ctx.multi_ret_vreg);
        fa.ops[1] = mop_slot(rslot); fa.ops[2] = mop_imm(0);
        emit(&ctx, fa);
        first_p = 1u;
    }

    uint32_t np = (fn->param_count > first_p) ? (fn->param_count - first_p) : 0u;
    if (np > 0u) {
        bool     *p_fp = (bool *)calloc(np, sizeof(bool));
        uint8_t  *p_w  = (uint8_t *)calloc(np, sizeof(uint8_t));
        uint32_t *p_al = (uint32_t *)calloc(np, sizeof(uint32_t));
        bool     *p_fs = (bool *)calloc(np, sizeof(bool));
        uint8_t  *p_gw = (uint8_t *)calloc(np, sizeof(uint8_t));
        T2ArgLoc *ploc = (T2ArgLoc *)calloc(np, sizeof(T2ArgLoc));
        if (!p_fp || !p_w || !p_al || !p_fs || !p_gw || !ploc) abort();

        for (uint32_t i = 0u; i < np; ++i) {
            const PirParamDecl *pd = &fn->params[first_p + i];
            p_fp[i] = t2_is_fp_type(pd->type);
            p_w[i]  = t2_val_width(pd->type);
            p_al[i] = pd->abi_stack_align;
            p_fs[i] = pd->abi_force_stack;
            p_gw[i] = pd->abi_group_words;
        }
        uint32_t gp_used = 0u;
        uint64_t fixed_stk = t2_plan_args(np, fn->is_variadic, target->caps.has_hw_fp,
                                          p_fp, p_w, p_al, p_fs, p_gw, ploc, &gp_used);
        out_mf->va_fixed_gp          = gp_used;
        out_mf->va_fixed_stack_bytes = fixed_stk;

        /* 1. Capture GP register params first (r0..r3). */
        MVReg gp_cap[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
        for (uint32_t r = 0u; r < 4u; ++r) {
            bool needed = false;
            for (uint32_t i = 0u; i < np; ++i) {
                if (ploc[i].gp_reg == (int8_t)r || ploc[i].gp_reg_hi == (int8_t)r) { needed = true; break; }
            }
            if (needed) {
                gp_cap[r] = new_gp(&ctx);
                MInst cp = minst_make(MI_COPY, 4u, 0u);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(gp_cap[r]); cp.ops[1] = mop_preg_use(T2_GP(r));
                emit(&ctx, cp);
            }
        }

        /* 2. Capture VFP register params (d0..d7) simultaneously before unpacking! */
        MVReg d_cap[8];
        uint8_t d_cap_w[8];
        for (uint32_t d = 0u; d < 8u; ++d) { d_cap[d] = MVREG_NONE; d_cap_w[d] = 0u; }
        for (uint32_t i = 0u; i < np; ++i) {
            if (ploc[i].vfp_s < 0) continue;
            uint32_t s = (uint32_t)ploc[i].vfp_s;
            uint32_t d = s / 2u;
            uint8_t need_w = (p_w[i] == 8u || (s & 1u) != 0u) ? 8u : 4u;
            if (need_w > d_cap_w[d]) d_cap_w[d] = need_w;
        }
        for (uint32_t d = 0u; d < 8u; ++d) {
            if (d_cap_w[d] > 0u) {
                d_cap[d] = new_fp(&ctx, d_cap_w[d]);
                MInst cp = minst_make(MI_COPY, d_cap_w[d], 0u);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(d_cap[d]); cp.ops[1] = mop_preg_use(T2_D(d));
                emit(&ctx, cp);
            }
        }

        /* 3. Distribute captured regs & load incoming stack params. */
        for (uint32_t i = 0u; i < np; ++i) {
            const PirParamDecl *pd = &fn->params[first_p + i];
            MVReg dst = ctx.val_vreg[pd->val_id];
            if (ploc[i].gp_reg >= 0) {
                if (p_fp[i] && p_w[i] == 8u && ploc[i].gp_reg_hi >= 0) {
                    MInst mv = minst_make(MI_T2_VMOV_G2D, 8u, 0u);
                    mv.op_count = 3u; mv.ops[0] = mop_vreg_def(dst);
                    mv.ops[1] = mop_vreg_use(gp_cap[ploc[i].gp_reg]);
                    mv.ops[2] = mop_vreg_use(gp_cap[ploc[i].gp_reg_hi]);
                    emit(&ctx, mv);
                } else if (p_fp[i] && p_w[i] == 4u) {
                    MInst mv = minst_make(MI_T2_VMOV_G2S, 4u, 0u);
                    mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst);
                    mv.ops[1] = mop_vreg_use(gp_cap[ploc[i].gp_reg]);
                    emit(&ctx, mv);
                } else {
                    emit_copy(&ctx, dst, gp_cap[ploc[i].gp_reg], 4u, 0u);
                }
            } else if (ploc[i].vfp_s >= 0) {
                uint32_t s = (uint32_t)ploc[i].vfp_s;
                uint32_t d = s / 2u;
                if (p_w[i] == 8u) {
                    emit_copy(&ctx, dst, d_cap[d], 8u, 0u);
                } else if (d_cap_w[d] == 4u && (s & 1u) == 0u) {
                    emit_copy(&ctx, dst, d_cap[d], 4u, 0u);
                } else {
                    MInst up = minst_make(MI_T2_UNPACK_S, 4u, 0u);
                    up.aux_width = (uint8_t)(s & 1u);
                    up.op_count  = 2u; up.ops[0] = mop_vreg_def(dst); up.ops[1] = mop_vreg_use(d_cap[d]);
                    emit(&ctx, up);
                }
            } else if (ploc[i].soff >= 0) {
                MInst ld = minst_make(MI_LOAD_INARG, ploc[i].byte_size, 0u);
                ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_imm(ploc[i].soff);
                emit(&ctx, ld);
            }
        }

        free(p_fp); free(p_w); free(p_al); free(p_fs); free(p_gw); free(ploc);
    }

    for (uint32_t b = 0u; b < nb && ctx.ok; ++b) {
        ctx.cur_mblk = ctx.blk_map[b];
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len && ctx.ok; ++j) {
            PirInstId iid = blk->insts.data[j];
            isel_inst(&ctx, iid, &fn->insts.data[iid]);
        }
    }

    free(ctx.val_vreg);
    free(ctx.alloca_slot);
    free(ctx.blk_map);
    return ctx.ok;
}
