/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * PowerPC 32-bit Big-Endian (SysV SVR4) Instruction Selection (LIR -> MIR).
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
    /* Lazy 8-byte aligned scratch slot in frame for GP<->FP conversions & bitcasts. */
    uint32_t  conv_slot;
    bool      has_conv_slot;
} PpcISelCtx;

static bool fits_simm16(int64_t v) {
    return v >= -32768 && v <= 32767;
}

static bool fits_uimm16(uint32_t v) {
    return v <= 65535u;
}

static MRegClass ppc_rclass(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR &&
        (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64)) {
        return MREG_CLASS_FP;
    }
    return MREG_CLASS_GP;
}

static uint8_t ppc_vreg_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        if (ty.u.scalar == PIR_SCALAR_F64) return 8u;
        if (ty.u.scalar == PIR_SCALAR_F32) return 4u;
    }
    return 4u;
}

static uint8_t ppc_mem_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:  return 1u;
            case PIR_SCALAR_I16: return 2u;
            case PIR_SCALAR_I32: case PIR_SCALAR_F32: case PIR_SCALAR_PTR: return 4u;
            case PIR_SCALAR_F64: return 8u;
            case PIR_SCALAR_I64: case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST: case PIR_SCALAR_VOID:
                break;
        }
    }
    return 4u;
}

static void emit(PpcISelCtx *c, MInst ins) {
    mblock_push(&c->mf->blocks.data[c->cur_mblk], ins);
}

static MVReg new_gp(PpcISelCtx *c) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, 4u);
}

static MVReg new_fp(PpcISelCtx *c, uint8_t w) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_FP, w);
}

static uint32_t ensure_conv_slot(PpcISelCtx *c) {
    if (!c->has_conv_slot) {
        c->conv_slot     = mfunc_add_slot(c->mf, 8u, 8u, false);
        c->has_conv_slot = true;
    }
    return c->conv_slot;
}

static MVReg emit_conv_slot_addr(PpcISelCtx *c, int64_t extra_off, uint32_t loc) {
    uint32_t sid = ensure_conv_slot(c);
    MVReg p = new_gp(c);
    MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
    fa.op_count = 3u;
    fa.ops[0] = mop_vreg_def(p);
    fa.ops[1] = mop_slot(sid);
    fa.ops[2] = mop_imm(extra_off);
    emit(c, fa);
    return p;
}

static uint32_t eval_imm_u32(const PirOperand *op) {
    uint64_t raw = op->u.imm_int.is_negative ? (UINT64_C(0) - op->u.imm_int.lo) : op->u.imm_int.lo;
    return (uint32_t)raw;
}

static void emit_li(PpcISelCtx *c, MVReg dst, int32_t val, uint32_t loc) {
    MInst li = minst_make(MI_PPC_LI, 4u, loc);
    li.op_count = 2u;
    li.ops[0] = mop_vreg_def(dst);
    li.ops[1] = mop_imm((int64_t)val);
    emit(c, li);
}

static void emit_imm_fp(PpcISelCtx *c, MVReg dst, uint64_t bits, uint8_t w, uint32_t loc) {
    MInst lf = minst_make(MI_PPC_LOAD_IMM_FP, w, loc);
    lf.op_count = 2u;
    lf.ops[0] = mop_vreg_def(dst);
    lf.ops[1] = mop_imm((int64_t)bits);
    emit(c, lf);
}

static void lookup_symbol_meta(const PirModule *mod, const char *name,
                               bool *out_is_func, bool *out_is_tls, bool *out_use_got) {
    *out_is_func = false;
    *out_is_tls  = false;
    *out_use_got = false;
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) {
            *out_is_func = true;
            *out_use_got = (mod->funcs.data[i].linkage == PIR_LINK_EXTERN ||
                            mod->funcs.data[i].linkage == PIR_LINK_WEAK);
            return;
        }
    }
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        if (strcmp(mod->globals.data[i].name, name) == 0) {
            *out_is_tls  = (mod->globals.data[i].seg == PIR_GSEG_TLS);
            *out_use_got = (mod->globals.data[i].linkage == PIR_LINK_EXTERN ||
                            mod->globals.data[i].linkage == PIR_LINK_WEAK);
            return;
        }
    }
    *out_use_got = true;
}

static void emit_sym_addr(PpcISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false, use_got = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls, &use_got);

    MVReg base = (offset != 0) ? new_gp(c) : dst;
    MInst a = minst_make(is_tls ? MI_PPC_TLS_ADDR : MI_PPC_SYM_ADDR, 4u, loc);
    a.op_count = 2u;
    a.ops[0]   = mop_vreg_def(base);
    a.ops[1]   = mop_sym(name, 0, is_func, is_tls, use_got);
    emit(c, a);

    if (offset != 0) {
        int32_t off32 = (int32_t)offset;
        if (fits_simm16(off32)) {
            MInst ad = minst_make(MI_PPC_ADDI, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_imm(off32);
            emit(c, ad);
        } else {
            MVReg off_r = new_gp(c);
            emit_li(c, off_r, off32, loc);
            MInst ad = minst_make(MI_PPC_ADD, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_vreg_use(off_r);
            emit(c, ad);
        }
    }
}

static MVReg mat_op(PpcISelCtx *c, const PirOperand *op, uint32_t loc) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            return c->val_vreg[op->u.val];
        case PIR_OPER_IMM_INT: {
            MVReg r = new_gp(c);
            emit_li(c, r, (int32_t)eval_imm_u32(op), loc);
            return r;
        }
        case PIR_OPER_IMM_FLOAT: {
            uint8_t w = ppc_vreg_bytes(op->type);
            MVReg fp = new_fp(c, w);
            emit_imm_fp(c, fp, op->u.imm_float.bits, w, loc);
            return fp;
        }
        case PIR_OPER_SYM: {
            MVReg r = new_gp(c);
            emit_sym_addr(c, r, op->u.sym.name, op->u.sym.offset, loc);
            return r;
        }
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            abort();
    }
    abort();
}

static MVReg emit_sext_sub(PpcISelCtx *c, MVReg src, uint8_t bits, uint32_t loc) {
    MVReg r = new_gp(c);
    MInst e = minst_make(bits == 8u ? MI_PPC_EXTSB : MI_PPC_EXTSH, 4u, loc);
    e.op_count = 2u; e.ops[0] = mop_vreg_def(r); e.ops[1] = mop_vreg_use(src);
    emit(c, e);
    return r;
}

static MVReg emit_zext_sub(PpcISelCtx *c, MVReg src, uint8_t bits, uint32_t loc) {
    MVReg r = new_gp(c);
    MInst a = minst_make(MI_PPC_ANDI, 4u, loc);
    a.op_count = 3u; a.ops[0] = mop_vreg_def(r); a.ops[1] = mop_vreg_use(src);
    a.ops[2] = mop_imm(bits == 8u ? 0xFF : 0xFFFF);
    emit(c, a);
    return r;
}

static MVReg normalize_int(PpcISelCtx *c, MVReg src, PirType ty, bool is_signed, uint32_t loc) {
    if (ty.kind != PIR_TYPE_SCALAR) return src;
    if (ty.u.scalar == PIR_SCALAR_I8) {
        return is_signed ? emit_sext_sub(c, src, 8u, loc) : emit_zext_sub(c, src, 8u, loc);
    }
    if (ty.u.scalar == PIR_SCALAR_I16) {
        return is_signed ? emit_sext_sub(c, src, 16u, loc) : emit_zext_sub(c, src, 16u, loc);
    }
    return src;
}

/* Emit CR0 conditional trap: if (cond) trap! */
static void emit_bc_trap(PpcISelCtx *c, MCond cond, uint32_t loc) {
    uint32_t trap_blk = mfunc_new_block(c->mf, "trap");
    uint32_t ok_blk   = mfunc_new_block(c->mf, "ok");

    MInst bc = minst_make(MI_PPC_BC, 4u, loc);
    bc.op_count = 2u;
    bc.ops[0] = mop_cond(cond);
    bc.ops[1] = mop_block(trap_blk);
    emit(c, bc);

    MInst j_ok = minst_make(MI_PPC_B, 4u, loc);
    j_ok.op_count = 1u; j_ok.ops[0] = mop_block(ok_blk);
    emit(c, j_ok);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_blk);
    mfunc_add_edge(c->mf, c->cur_mblk, ok_blk);

    MInst tr = minst_make(MI_PPC_TRAP, 4u, loc);
    mblock_push(&c->mf->blocks.data[trap_blk], tr);

    c->cur_mblk = ok_blk;
}

static void emit_cmp_rr(PpcISelCtx *c, bool is_unsigned, MVReg a, MVReg b, uint32_t loc) {
    MInst cm = minst_make(is_unsigned ? MI_PPC_CMPLW : MI_PPC_CMPW, 4u, loc);
    cm.op_count = 2u; cm.ops[0] = mop_vreg_use(a); cm.ops[1] = mop_vreg_use(b);
    emit(c, cm);
}

static void emit_cmp_ri(PpcISelCtx *c, bool is_unsigned, MVReg a, int32_t imm, uint32_t loc) {
    MInst cm = minst_make(is_unsigned ? MI_PPC_CMPLWI : MI_PPC_CMPWI, 4u, loc);
    cm.op_count = 2u; cm.ops[0] = mop_vreg_use(a); cm.ops[1] = mop_imm(imm);
    emit(c, cm);
}

static void emit_setcc(PpcISelCtx *c, MVReg dst, MCond cond, uint32_t loc) {
    MInst sc = minst_make(MI_PPC_SETCC, 4u, loc);
    sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst); sc.ops[1] = mop_cond(cond);
    emit(c, sc);
}

/* =========================================================================
 * Parallel Copy Emission for Block Arguments
 * ========================================================================= */

static void emit_branch_pcopy(PpcISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    if (bt->arg_count == 0u) return;
    const PirBlock *dst_blk = &c->fn->blocks.data[bt->target];
    uint32_t n = bt->arg_count;
    MPCopyEntry *entries = (MPCopyEntry *)calloc(n, sizeof(MPCopyEntry));
    if (!entries) abort();

    for (uint32_t k = 0u; k < n; ++k) {
        const PirOperand *arg = &bt->args[k];
        PirValueId pv = dst_blk->params.data[k];
        entries[k].dst = c->val_vreg[pv];
        if (arg->kind == PIR_OPER_IMM_INT) {
            entries[k].src_is_imm = true;
            entries[k].imm_bits   = (uint64_t)eval_imm_u32(arg);
        } else if (arg->kind == PIR_OPER_IMM_FLOAT) {
            entries[k].src_is_imm = true;
            entries[k].imm_bits   = arg->u.imm_float.bits;
        } else {
            entries[k].src_is_imm = false;
            entries[k].src        = mat_op(c, arg, loc);
        }
    }
    uint32_t pc_id = mfunc_add_pcopy(c->mf, entries, n);
    free(entries);

    MInst pc = minst_make(MI_PCOPY, 0u, loc);
    pc.aux_id = pc_id;
    emit(c, pc);
}

static uint32_t resolve_edge_target(PpcISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    uint32_t dst_mb = c->blk_map[bt->target];
    if (bt->arg_count == 0u) return dst_mb;
    uint32_t saved_mb = c->cur_mblk;
    uint32_t tramp = mfunc_new_block(c->mf, "edge");
    c->cur_mblk = tramp;
    emit_branch_pcopy(c, bt, loc);
    MInst j = minst_make(MI_PPC_B, 4u, loc);
    j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
    emit(c, j);
    mfunc_add_edge(c->mf, tramp, dst_mb);
    c->cur_mblk = saved_mb;
    return tramp;
}

/* =========================================================================
 * Integer <-> Floating-Point Conversions (Base PPC32 IEEE-754)
 * ========================================================================= */

static void emit_itof_f64(PpcISelCtx *c, MVReg dst_f64, MVReg src_gp, bool is_signed, uint32_t loc) {
    MVReg p0 = emit_conv_slot_addr(c, 0, loc);
    MVReg hi = new_gp(c);
    emit_li(c, hi, (int32_t)0x43300000u, loc);

    MVReg lo = src_gp;
    if (is_signed) {
        /* Flip sign bit: lo = src ^ 0x80000000 */
        MVReg sign_b = new_gp(c);
        emit_li(c, sign_b, (int32_t)0x80000000u, loc);
        lo = new_gp(c);
        MInst xr = minst_make(MI_PPC_XOR, 4u, loc);
        xr.op_count = 3u; xr.ops[0] = mop_vreg_def(lo); xr.ops[1] = mop_vreg_use(src_gp); xr.ops[2] = mop_vreg_use(sign_b);
        emit(c, xr);
    }

    /* Big-endian 8B double slot: hi word at +0, lo word at +4 */
    MInst st0 = minst_make(MI_PPC_ST, 4u, loc);
    st0.op_count = 3u; st0.ops[0] = mop_vreg_use(hi); st0.ops[1] = mop_vreg_use(p0); st0.ops[2] = mop_imm(0);
    emit(c, st0);
    MInst st1 = minst_make(MI_PPC_ST, 4u, loc);
    st1.op_count = 3u; st1.ops[0] = mop_vreg_use(lo); st1.ops[1] = mop_vreg_use(p0); st1.ops[2] = mop_imm(4);
    emit(c, st1);

    MVReg fraw = new_fp(c, 8u);
    MInst ld = minst_make(MI_PPC_FLD, 8u, loc);
    ld.op_count = 3u; ld.ops[0] = mop_vreg_def(fraw); ld.ops[1] = mop_vreg_use(p0); ld.ops[2] = mop_imm(0);
    emit(c, ld);

    MVReg fbias = new_fp(c, 8u);
    uint64_t bias_bits = is_signed ? UINT64_C(0x4330000080000000) : UINT64_C(0x4330000000000000);
    emit_imm_fp(c, fbias, bias_bits, 8u, loc);

    MInst sub = minst_make(MI_PPC_FSUB, 8u, loc);
    sub.op_count = 3u; sub.ops[0] = mop_vreg_def(dst_f64);
    sub.ops[1] = mop_vreg_use(fraw); sub.ops[2] = mop_vreg_use(fbias);
    emit(c, sub);
}

static void emit_fctiwz_to_gp(PpcISelCtx *c, MVReg dst_gp, MVReg src_fp, uint32_t loc) {
    MVReg ft = new_fp(c, 8u);
    MInst cv = minst_make(MI_PPC_FCTIWZ, 8u, loc);
    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(ft); cv.ops[1] = mop_vreg_use(src_fp);
    emit(c, cv);

    MVReg p0 = emit_conv_slot_addr(c, 0, loc);
    MInst st = minst_make(MI_PPC_FST, 8u, loc);
    st.op_count = 3u; st.ops[0] = mop_vreg_use(ft); st.ops[1] = mop_vreg_use(p0); st.ops[2] = mop_imm(0);
    emit(c, st);

    /* Big-endian: 32-bit integer result of fctiwz lives in low 32 bits at offset +4! */
    MInst ld = minst_make(MI_PPC_LD, 4u, loc);
    ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst_gp); ld.ops[1] = mop_vreg_use(p0); ld.ops[2] = mop_imm(4);
    emit(c, ld);
}

/* =========================================================================
 * Call Lowering (SysV PowerPC 32-bit SVR4 ABI)
 * ========================================================================= */

static void isel_call(PpcISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);
    const PirFunction *cal_fn = NULL;
    const PirSigDef   *cal_sig = NULL;
    PirCabiRetInfo cabi_ret;
    memset(&cabi_ret, 0, sizeof(cabi_ret));
    PirType ret_ty = inst->ty;
    bool is_variadic = false;

    if (!is_ind && inst->operands[0].u.sym.is_func) {
        cal_fn      = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        cabi_ret    = cal_fn->cabi_ret;
        ret_ty      = cal_fn->ret_type;
        is_variadic = cal_fn->is_variadic;
    } else if (is_ind && inst->sig_id < c->mod->sigs.len) {
        cal_sig     = &c->mod->sigs.data[inst->sig_id];
        cabi_ret    = cal_sig->cabi_ret;
        ret_ty      = cal_sig->ret_type;
        is_variadic = cal_sig->is_variadic;
    }

    c->mf->has_calls = true;
    uint32_t first_arg = 1u;
    MVReg hidden_out_v = MVREG_NONE;
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) {
        hidden_out_v = mat_op(c, &inst->operands[1], loc);
        first_arg = 2u;
    }

    uint32_t nargs = inst->operand_count - first_arg;
    MVReg   *arg_v = nargs ? (MVReg *)calloc(nargs, sizeof(MVReg)) : NULL;
    bool    *is_fp = nargs ? (bool *)calloc(nargs, sizeof(bool)) : NULL;
    bool    *is_f32_va = nargs ? (bool *)calloc(nargs, sizeof(bool)) : NULL;
    uint8_t *arg_w = nargs ? (uint8_t *)calloc(nargs, sizeof(uint8_t)) : NULL;
    if (nargs && (!arg_v || !is_fp || !is_f32_va || !arg_w)) abort();

    uint32_t fixed_n = cal_fn ? cal_fn->param_count : (cal_sig ? cal_sig->param_count : 0u);
    uint32_t fixed_offset = (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) ? 1u : 0u;

    for (uint32_t i = 0u; i < nargs; ++i) {
        const PirOperand *aop = &inst->operands[first_arg + i];
        arg_v[i] = mat_op(c, aop, loc);
        bool fp = (ppc_rclass(aop->type) == MREG_CLASS_FP);
        bool va_pos = is_variadic && (i + fixed_offset) >= fixed_n;
        bool prom_f32 = va_pos && fp && aop->type.u.scalar == PIR_SCALAR_F32 && !aop->call_no_f32_promote;
        is_fp[i]     = fp;
        is_f32_va[i] = prom_f32;
        arg_w[i]     = prom_f32 ? 8u : ppc_vreg_bytes(aop->type);
        /* For sub-word integer args, normalize appropriately (C vararg promotes to int). */
        if (!fp && aop->type.kind == PIR_TYPE_SCALAR &&
            (aop->type.u.scalar == PIR_SCALAR_I8 || aop->type.u.scalar == PIR_SCALAR_I16)) {
            bool sign_ext = false;
            if (!va_pos) {
                const PirParamDecl *pd = NULL;
                if (cal_fn && (i + fixed_offset) < cal_fn->param_count) pd = &cal_fn->params[i + fixed_offset];
                else if (cal_sig && (i + fixed_offset) < cal_sig->param_count) pd = &cal_sig->params[i + fixed_offset];
                if (pd && (pd->attrs & PIR_PATTR_SIGNEXT) != 0u) sign_ext = true;
            }
            arg_v[i] = normalize_int(c, arg_v[i], aop->type, sign_ext, loc);
        }
    }

    /* Plan SysV PPC32 register & stack slots (stack starts at sp + 8, offset 0 here -> +8 in frame). */
    MPReg    *arg_preg = nargs ? (MPReg *)calloc(nargs, sizeof(MPReg)) : NULL;
    int64_t  *arg_soff = nargs ? (int64_t *)calloc(nargs, sizeof(int64_t)) : NULL;
    if (nargs && (!arg_preg || !arg_soff)) abort();

    uint32_t gp_i = 0u, fp_i = 0u;
    uint64_t stk_off = 0u;

    for (uint32_t i = 0u; i < nargs; ++i) {
        const PirOperand *aop = &inst->operands[first_arg + i];
        arg_preg[i] = MREG_NONE;
        arg_soff[i] = -1;

        if (is_fp[i]) {
            if (!aop->call_force_stack && fp_i < 8u) {
                arg_preg[i] = PPC_FP(1u + fp_i);
                fp_i++;
            } else {
                uint64_t al = (arg_w[i] >= 8u || aop->call_stack_align >= 8u) ? 8u : 4u;
                stk_off = (stk_off + al - 1u) & ~(al - 1u);
                arg_soff[i] = (int64_t)stk_off;
                stk_off += arg_w[i];
            }
        } else {
            bool need_al8 = (aop->call_stack_align >= 8u);
            if (!aop->call_force_stack) {
                if (need_al8 && (gp_i & 1u) != 0u && gp_i < 8u) {
                    gp_i++;
                }
                /* If this is the first half of an 8B-aligned pair and only 1 reg (r10) remains,
                 * SysV PPC32 pushes BOTH halves onto 8B-aligned stack. */
                if (need_al8 && gp_i + 2u > 8u) {
                    gp_i = 8u;
                }
            }
            if (!aop->call_force_stack && gp_i < 8u) {
                arg_preg[i] = PPC_GP(3u + gp_i);
                gp_i++;
            } else {
                uint64_t al = need_al8 ? 8u : 4u;
                stk_off = (stk_off + al - 1u) & ~(al - 1u);
                arg_soff[i] = (int64_t)stk_off;
                stk_off += 4u;
            }
        }
    }

    uint64_t out_bytes = (stk_off + 15u) & ~UINT64_C(15);
    if (out_bytes > c->mf->max_out_args_bytes) c->mf->max_out_args_bytes = out_bytes;

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_DOWN, 4u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    /* Emit stack stores first (so arg regs aren't live yet). */
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (arg_soff[i] < 0) continue;
        MInst st = minst_make(MI_STORE_OUTARG, arg_w[i], loc);
        st.op_count = 2u;
        st.ops[0] = mop_vreg_use(arg_v[i]);
        st.ops[1] = mop_imm(arg_soff[i]);
        emit(c, st);
    }

    /* Indirect target pinned to r10? No — if gp_i == 8, r10 is an arg reg!
     * Pin indirect target to a scratch-safe callee-not-clobbered? Wait: in MI_PPC_CALL_IND,
     * `mtctr rS; bctrl` runs AFTER arg regs are loaded, so rS must NOT collide with r3..r10!
     * Putting rS into a dedicated vreg (allocated to r14..r29, or if spilled, reloaded into r11)
     * BEFORE arg regs are fixed-pinned guarantees rS NEVER collides with r3..r10! */
    MVReg ind_v = MVREG_NONE;
    if (is_ind) {
        ind_v = mat_op(c, &inst->operands[0], loc);
    }

    /* Copy register arguments into fixed physical arg regs r3..r10 / f1..f8. */
    for (uint32_t i = 0u; i < nargs; ++i) {
        if (arg_preg[i] == MREG_NONE) continue;
        MVReg fv = is_fp[i] ? new_fp(c, arg_w[i]) : new_gp(c);
        c->mf->vregs.data[fv].fixed_preg = arg_preg[i];
        MInst cp = minst_make(MI_COPY, arg_w[i], loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(fv); cp.ops[1] = mop_vreg_use(arg_v[i]);
        emit(c, cp);
    }

    if (is_ind) {
        MInst ci = minst_make(MI_PPC_CALL_IND, 4u, loc);
        ci.is_call  = true;
        ci.aux_id   = (is_variadic && fp_i > 0u) ? 1u : 0u;
        ci.op_count = 1u;
        ci.ops[0]   = mop_vreg_use(ind_v);
        emit(c, ci);
    } else {
        MInst cl = minst_make(MI_PPC_CALL, 4u, loc);
        cl.is_call  = true;
        cl.aux_id   = (is_variadic && fp_i > 0u) ? 1u : 0u;
        cl.op_count = 1u;
        cl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, cl);
    }

    if (out_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_UP, 4u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)out_bytes);
        emit(c, adj);
    }

    free(arg_v);
    free(is_fp);
    free(is_f32_va);
    free(arg_w);
    free(arg_preg);
    free(arg_soff);

    /* Capture return value(s). */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) {
        uint32_t nregs = cabi_ret.reg_count;
        MVReg saved_v[2] = { MVREG_NONE, MVREG_NONE };
        for (uint32_t k = 0u; k < nregs && k < 2u; ++k) {
            saved_v[k] = new_gp(c);
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u;
            cp.ops[0] = mop_vreg_def(saved_v[k]);
            cp.ops[1] = mop_preg_use(PPC_GP(3u + k));
            emit(c, cp);
        }
        uint64_t agg_sz = cabi_ret.agg_size > 0u ? cabi_ret.agg_size : ((uint64_t)nregs * 4u);
        for (uint32_t k = 0u; k < nregs && k < 2u; ++k) {
            uint64_t off = (uint64_t)k * 4u;
            uint64_t rem = (agg_sz > off) ? (agg_sz - off) : 0u;
            if (rem == 0u) continue;
            if (rem < 4u) {
                /* Big-endian partial trailing word: byte 0 is MSB (bits 24..31), etc. */
                for (uint64_t b = 0u; b < rem; ++b) {
                    MVReg bv = saved_v[k];
                    uint32_t sh = (uint32_t)((3u - b) * 8u);
                    if (sh > 0u) {
                        bv = new_gp(c);
                        MInst sr = minst_make(MI_PPC_SRWI, 4u, loc);
                        sr.op_count = 3u; sr.ops[0] = mop_vreg_def(bv);
                        sr.ops[1] = mop_vreg_use(saved_v[k]); sr.ops[2] = mop_imm((int64_t)sh);
                        emit(c, sr);
                    }
                    MInst sb = minst_make(MI_PPC_ST, 1u, loc);
                    sb.op_count = 3u; sb.ops[0] = mop_vreg_use(bv);
                    sb.ops[1] = mop_vreg_use(hidden_out_v); sb.ops[2] = mop_imm((int64_t)(off + b));
                    emit(c, sb);
                }
            } else {
                MInst st = minst_make(MI_PPC_ST, 4u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(saved_v[k]);
                st.ops[1] = mop_vreg_use(hidden_out_v);
                st.ops[2] = mop_imm((int64_t)off);
                emit(c, st);
            }
        }
        return;
    }

    if (inst->results[0] != PIR_INVALID_ID &&
        !(ret_ty.kind == PIR_TYPE_SCALAR && ret_ty.u.scalar == PIR_SCALAR_VOID)) {
        MVReg dst = c->val_vreg[inst->results[0]];
        MPReg rpreg = (ppc_rclass(ret_ty) == MREG_CLASS_FP) ? PPC_F1 : PPC_R3;
        MInst cp = minst_make(MI_COPY, ppc_vreg_bytes(ret_ty), loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(rpreg);
        emit(c, cp);
    }
}

/* =========================================================================
 * Single Instruction Selection
 * ========================================================================= */

static void isel_inst(PpcISelCtx *c, const PirInst *inst) {
    uint32_t loc = inst->loc_idx;

    switch (inst->op) {
        case PIR_OP_CONST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            if (inst->operands[0].kind == PIR_OPER_IMM_FLOAT) {
                emit_imm_fp(c, dst, inst->operands[0].u.imm_float.bits, ppc_vreg_bytes(inst->ty), loc);
            } else if (inst->operands[0].kind == PIR_OPER_SYM) {
                emit_sym_addr(c, dst, inst->operands[0].u.sym.name, inst->operands[0].u.sym.offset, loc);
            } else {
                emit_li(c, dst, (int32_t)eval_imm_u32(&inst->operands[0]), loc);
            }
            return;
        }

        case PIR_OP_ADDR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            emit_sym_addr(c, dst, inst->operands[0].u.sym.name, inst->operands[0].u.sym.offset, loc);
            return;
        }

        case PIR_OP_ADD:
        case PIR_OP_SUB: {
            MVReg dst = c->val_vreg[inst->results[0]];
            PirScalarType sc = inst->ty.u.scalar;
            bool is_add = (inst->op == PIR_OP_ADD);
            bool trap_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
            bool trap_u = (inst->flags & PIR_FLAG_TRAP_U) != 0u;

            if (!trap_s && !trap_u) {
                MVReg lhs = mat_op(c, &inst->operands[0], loc);
                if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                    int32_t imm = (int32_t)eval_imm_u32(&inst->operands[1]);
                    int64_t adj = is_add ? (int64_t)imm : -(int64_t)imm;
                    if (fits_simm16(adj)) {
                        MInst ai = minst_make(MI_PPC_ADDI, 4u, loc);
                        ai.op_count = 3u; ai.ops[0] = mop_vreg_def(dst);
                        ai.ops[1] = mop_vreg_use(lhs); ai.ops[2] = mop_imm(adj);
                        emit(c, ai);
                        return;
                    }
                }
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MInst mi = minst_make(is_add ? MI_PPC_ADD : MI_PPC_SUB, 4u, loc);
                mi.op_count = 3u; mi.ops[0] = mop_vreg_def(dst);
                mi.ops[1] = mop_vreg_use(lhs); mi.ops[2] = mop_vreg_use(rhs);
                emit(c, mi);
                return;
            }

            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, trap_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, trap_s, loc);
            MInst mi = minst_make(is_add ? MI_PPC_ADD : MI_PPC_SUB, 4u, loc);
            mi.op_count = 3u; mi.ops[0] = mop_vreg_def(dst);
            mi.ops[1] = mop_vreg_use(lhs); mi.ops[2] = mop_vreg_use(rhs);
            emit(c, mi);

            if (sc == PIR_SCALAR_I8 || sc == PIR_SCALAR_I16) {
                uint8_t bits = (sc == PIR_SCALAR_I8) ? 8u : 16u;
                MVReg chk = trap_s ? emit_sext_sub(c, dst, bits, loc) : emit_zext_sub(c, dst, bits, loc);
                emit_cmp_rr(c, false, dst, chk, loc);
                emit_bc_trap(c, MCOND_NE, loc);
            } else if (trap_u) {
                if (is_add) emit_cmp_rr(c, true, dst, lhs, loc);
                else        emit_cmp_rr(c, true, lhs, rhs, loc);
                emit_bc_trap(c, MCOND_LT, loc); /* unsigned < */
            } else {
                MVReg x1 = new_gp(c), x2 = new_gp(c), x3 = new_gp(c);
                MInst e1 = minst_make(MI_PPC_XOR, 4u, loc);
                e1.op_count = 3u; e1.ops[0] = mop_vreg_def(x1);
                e1.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                e1.ops[2] = mop_vreg_use(is_add ? lhs : rhs);
                emit(c, e1);
                MInst e2 = minst_make(MI_PPC_XOR, 4u, loc);
                e2.op_count = 3u; e2.ops[0] = mop_vreg_def(x2);
                e2.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                e2.ops[2] = mop_vreg_use(is_add ? rhs : dst);
                emit(c, e2);
                MInst a3 = minst_make(MI_PPC_AND, 4u, loc);
                a3.op_count = 3u; a3.ops[0] = mop_vreg_def(x3);
                a3.ops[1] = mop_vreg_use(x1); a3.ops[2] = mop_vreg_use(x2);
                emit(c, a3);
                emit_cmp_ri(c, false, x3, 0, loc);
                emit_bc_trap(c, MCOND_LT, loc);
            }
            return;
        }

        case PIR_OP_MUL: {
            MVReg dst = c->val_vreg[inst->results[0]];
            PirScalarType sc = inst->ty.u.scalar;
            bool trap_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
            bool trap_u = (inst->flags & PIR_FLAG_TRAP_U) != 0u;

            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, trap_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, trap_s, loc);

            MInst mul = minst_make(MI_PPC_MULLW, 4u, loc);
            mul.op_count = 3u; mul.ops[0] = mop_vreg_def(dst);
            mul.ops[1] = mop_vreg_use(lhs); mul.ops[2] = mop_vreg_use(rhs);
            emit(c, mul);

            if (!trap_s && !trap_u) return;

            if (sc == PIR_SCALAR_I8 || sc == PIR_SCALAR_I16) {
                uint8_t bits = (sc == PIR_SCALAR_I8) ? 8u : 16u;
                MVReg chk = trap_s ? emit_sext_sub(c, dst, bits, loc) : emit_zext_sub(c, dst, bits, loc);
                emit_cmp_rr(c, false, dst, chk, loc);
                emit_bc_trap(c, MCOND_NE, loc);
            } else if (trap_u) {
                MVReg hi = new_gp(c);
                MInst mh = minst_make(MI_PPC_MULHWU, 4u, loc);
                mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi);
                mh.ops[1] = mop_vreg_use(lhs); mh.ops[2] = mop_vreg_use(rhs);
                emit(c, mh);
                emit_cmp_ri(c, false, hi, 0, loc);
                emit_bc_trap(c, MCOND_NE, loc);
            } else {
                MVReg hi = new_gp(c), sgn = new_gp(c);
                MInst mh = minst_make(MI_PPC_MULHW, 4u, loc);
                mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi);
                mh.ops[1] = mop_vreg_use(lhs); mh.ops[2] = mop_vreg_use(rhs);
                emit(c, mh);
                MInst sr = minst_make(MI_PPC_SRAWI, 4u, loc);
                sr.op_count = 3u; sr.ops[0] = mop_vreg_def(sgn);
                sr.ops[1] = mop_vreg_use(dst); sr.ops[2] = mop_imm(31);
                emit(c, sr);
                emit_cmp_rr(c, false, hi, sgn, loc);
                emit_bc_trap(c, MCOND_NE, loc);
            }
            return;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            MVReg rlo = c->val_vreg[inst->results[0]];
            MVReg rhi = c->val_vreg[inst->results[1]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MInst mlo = minst_make(MI_PPC_MULLW, 4u, loc);
            mlo.op_count = 3u; mlo.ops[0] = mop_vreg_def(rlo);
            mlo.ops[1] = mop_vreg_use(lhs); mlo.ops[2] = mop_vreg_use(rhs);
            emit(c, mlo);
            MInst mhi = minst_make(inst->op == PIR_OP_MUL_WIDE_S ? MI_PPC_MULHW : MI_PPC_MULHWU, 4u, loc);
            mhi.op_count = 3u; mhi.ops[0] = mop_vreg_def(rhi);
            mhi.ops[1] = mop_vreg_use(lhs); mhi.ops[2] = mop_vreg_use(rhs);
            emit(c, mhi);
            return;
        }

        case PIR_OP_DIV_S:
        case PIR_OP_DIV_U:
        case PIR_OP_REM_S:
        case PIR_OP_REM_U: {
            MVReg dst = c->val_vreg[inst->results[0]];
            bool is_s   = (inst->op == PIR_OP_DIV_S || inst->op == PIR_OP_REM_S);
            bool is_rem = (inst->op == PIR_OP_REM_S || inst->op == PIR_OP_REM_U);
            bool is_wrap = (inst->flags & PIR_FLAG_WRAP) != 0u;
            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, is_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, is_s, loc);

            if (!(inst->flags & PIR_FLAG_UNSAFE)) {
                emit_cmp_ri(c, false, rhs, 0, loc);
                emit_bc_trap(c, MCOND_EQ, loc);
            }

            int32_t min_v = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? INT8_MIN
                          : (inst->ty.u.scalar == PIR_SCALAR_I16) ? INT16_MIN : INT32_MIN;

            if (is_s && !is_wrap && !(inst->flags & PIR_FLAG_UNSAFE)) {
                MVReg cmin = new_gp(c), cm1 = new_gp(c);
                MVReg d1 = new_gp(c), d2 = new_gp(c), both = new_gp(c);
                emit_li(c, cmin, min_v, loc);
                emit_li(c, cm1, -1, loc);
                MInst x1 = minst_make(MI_PPC_XOR, 4u, loc);
                x1.op_count = 3u; x1.ops[0] = mop_vreg_def(d1); x1.ops[1] = mop_vreg_use(lhs); x1.ops[2] = mop_vreg_use(cmin);
                emit(c, x1);
                MInst x2 = minst_make(MI_PPC_XOR, 4u, loc);
                x2.op_count = 3u; x2.ops[0] = mop_vreg_def(d2); x2.ops[1] = mop_vreg_use(rhs); x2.ops[2] = mop_vreg_use(cm1);
                emit(c, x2);
                MInst o1 = minst_make(MI_PPC_OR, 4u, loc);
                o1.op_count = 3u; o1.ops[0] = mop_vreg_def(both); o1.ops[1] = mop_vreg_use(d1); o1.ops[2] = mop_vreg_use(d2);
                emit(c, o1);
                emit_cmp_ri(c, false, both, 0, loc);
                emit_bc_trap(c, MCOND_EQ, loc);
            }

            if (is_s && is_wrap && inst->ty.u.scalar == PIR_SCALAR_I32) {
                /* PPC divw(INT32_MIN, -1) is undefined! Branch around it for [wrap]. */
                uint32_t ovf_b  = mfunc_new_block(c->mf, "div_ovf");
                uint32_t norm_b = mfunc_new_block(c->mf, "div_norm");
                uint32_t done_b = mfunc_new_block(c->mf, "div_done");

                MVReg cmin = new_gp(c), cm1 = new_gp(c);
                MVReg d1 = new_gp(c), d2 = new_gp(c), both = new_gp(c);
                emit_li(c, cmin, INT32_MIN, loc);
                emit_li(c, cm1, -1, loc);
                MInst x1 = minst_make(MI_PPC_XOR, 4u, loc);
                x1.op_count = 3u; x1.ops[0] = mop_vreg_def(d1); x1.ops[1] = mop_vreg_use(lhs); x1.ops[2] = mop_vreg_use(cmin);
                emit(c, x1);
                MInst x2 = minst_make(MI_PPC_XOR, 4u, loc);
                x2.op_count = 3u; x2.ops[0] = mop_vreg_def(d2); x2.ops[1] = mop_vreg_use(rhs); x2.ops[2] = mop_vreg_use(cm1);
                emit(c, x2);
                MInst o1 = minst_make(MI_PPC_OR, 4u, loc);
                o1.op_count = 3u; o1.ops[0] = mop_vreg_def(both); o1.ops[1] = mop_vreg_use(d1); o1.ops[2] = mop_vreg_use(d2);
                emit(c, o1);
                emit_cmp_ri(c, false, both, 0, loc);

                MInst bc = minst_make(MI_PPC_BC, 4u, loc);
                bc.op_count = 2u; bc.ops[0] = mop_cond(MCOND_EQ); bc.ops[1] = mop_block(ovf_b);
                emit(c, bc);
                MInst jn = minst_make(MI_PPC_B, 4u, loc);
                jn.op_count = 1u; jn.ops[0] = mop_block(norm_b);
                emit(c, jn);
                mfunc_add_edge(c->mf, c->cur_mblk, ovf_b);
                mfunc_add_edge(c->mf, c->cur_mblk, norm_b);

                c->cur_mblk = ovf_b;
                emit_li(c, dst, is_rem ? 0 : INT32_MIN, loc);
                MInst jd1 = minst_make(MI_PPC_B, 4u, loc);
                jd1.op_count = 1u; jd1.ops[0] = mop_block(done_b);
                emit(c, jd1);
                mfunc_add_edge(c->mf, ovf_b, done_b);

                c->cur_mblk = norm_b;
                MVReg q = is_rem ? new_gp(c) : dst;
                MInst dv = minst_make(MI_PPC_DIVW, 4u, loc);
                dv.op_count = 3u; dv.ops[0] = mop_vreg_def(q);
                dv.ops[1] = mop_vreg_use(lhs); dv.ops[2] = mop_vreg_use(rhs);
                emit(c, dv);
                if (is_rem) {
                    MVReg prod = new_gp(c);
                    MInst ml = minst_make(MI_PPC_MULLW, 4u, loc);
                    ml.op_count = 3u; ml.ops[0] = mop_vreg_def(prod);
                    ml.ops[1] = mop_vreg_use(q); ml.ops[2] = mop_vreg_use(rhs);
                    emit(c, ml);
                    MInst sb = minst_make(MI_PPC_SUB, 4u, loc);
                    sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst);
                    sb.ops[1] = mop_vreg_use(lhs); sb.ops[2] = mop_vreg_use(prod);
                    emit(c, sb);
                }
                MInst jd2 = minst_make(MI_PPC_B, 4u, loc);
                jd2.op_count = 1u; jd2.ops[0] = mop_block(done_b);
                emit(c, jd2);
                mfunc_add_edge(c->mf, norm_b, done_b);

                c->cur_mblk = done_b;
                return;
            }

            MVReg q = is_rem ? new_gp(c) : dst;
            MInst dv = minst_make(is_s ? MI_PPC_DIVW : MI_PPC_DIVWU, 4u, loc);
            dv.op_count = 3u; dv.ops[0] = mop_vreg_def(q);
            dv.ops[1] = mop_vreg_use(lhs); dv.ops[2] = mop_vreg_use(rhs);
            emit(c, dv);
            if (is_rem) {
                MVReg prod = new_gp(c);
                MInst ml = minst_make(MI_PPC_MULLW, 4u, loc);
                ml.op_count = 3u; ml.ops[0] = mop_vreg_def(prod);
                ml.ops[1] = mop_vreg_use(q); ml.ops[2] = mop_vreg_use(rhs);
                emit(c, ml);
                MInst sb = minst_make(MI_PPC_SUB, 4u, loc);
                sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst);
                sb.ops[1] = mop_vreg_use(lhs); sb.ops[2] = mop_vreg_use(prod);
                emit(c, sb);
            }
            return;
        }

        case PIR_OP_AND:
        case PIR_OP_OR:
        case PIR_OP_XOR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint32_t imm = eval_imm_u32(&inst->operands[1]);
                if (fits_uimm16(imm)) {
                    MOpcode iop = (inst->op == PIR_OP_AND) ? MI_PPC_ANDI
                                : (inst->op == PIR_OP_OR)  ? MI_PPC_ORI : MI_PPC_XORI;
                    MInst bi = minst_make(iop, 4u, loc);
                    bi.op_count = 3u; bi.ops[0] = mop_vreg_def(dst);
                    bi.ops[1] = mop_vreg_use(lhs); bi.ops[2] = mop_imm((int64_t)imm);
                    emit(c, bi);
                    return;
                }
            }
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MOpcode rop = (inst->op == PIR_OP_AND) ? MI_PPC_AND
                        : (inst->op == PIR_OP_OR)  ? MI_PPC_OR : MI_PPC_XOR;
            MInst br = minst_make(rop, 4u, loc);
            br.op_count = 3u; br.ops[0] = mop_vreg_def(dst);
            br.ops[1] = mop_vreg_use(lhs); br.ops[2] = mop_vreg_use(rhs);
            emit(c, br);
            return;
        }

        case PIR_OP_SHL:
        case PIR_OP_SHR:
        case PIR_OP_SAR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 4u);
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->op == PIR_OP_SHR) {
                lhs = normalize_int(c, lhs, inst->ty, false, loc);
            } else if (inst->op == PIR_OP_SAR) {
                lhs = normalize_int(c, lhs, inst->ty, true, loc);
            }
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint32_t sh = eval_imm_u32(&inst->operands[1]);
                if (inst->ty.u.scalar == PIR_SCALAR_I8)  sh &= 0xFFu;
                if (inst->ty.u.scalar == PIR_SCALAR_I16) sh &= 0xFFFFu;
                if ((inst->flags & PIR_FLAG_TRAP) && sh >= bits) {
                    MInst tr = minst_make(MI_PPC_TRAP, 4u, loc);
                    emit(c, tr);
                    return;
                }
                sh &= (bits - 1u);
                if (sh == 0u) {
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(lhs);
                    emit(c, cp);
                } else {
                    MOpcode sop = (inst->op == PIR_OP_SHL) ? MI_PPC_SLWI
                                : (inst->op == PIR_OP_SHR) ? MI_PPC_SRWI : MI_PPC_SRAWI;
                    MInst si = minst_make(sop, 4u, loc);
                    si.op_count = 3u; si.ops[0] = mop_vreg_def(dst);
                    si.ops[1] = mop_vreg_use(lhs); si.ops[2] = mop_imm((int64_t)sh);
                    emit(c, si);
                }
                return;
            }
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
            if (inst->flags & PIR_FLAG_TRAP) {
                emit_cmp_ri(c, true, rhs, (int32_t)bits, loc);
                emit_bc_trap(c, MCOND_GE, loc); /* unsigned >= bits */
            }
            /* PPC slw/srw/sraw examine 6 bits (0..63), so ALWAYS mask to (bits - 1)! */
            MVReg sh_m = new_gp(c);
            MInst ma = minst_make(MI_PPC_ANDI, 4u, loc);
            ma.op_count = 3u; ma.ops[0] = mop_vreg_def(sh_m);
            ma.ops[1] = mop_vreg_use(rhs); ma.ops[2] = mop_imm((int64_t)(bits - 1u));
            emit(c, ma);

            MOpcode sop = (inst->op == PIR_OP_SHL) ? MI_PPC_SLW
                        : (inst->op == PIR_OP_SHR) ? MI_PPC_SRW : MI_PPC_SRAW;
            MInst sr = minst_make(sop, 4u, loc);
            sr.op_count = 3u; sr.ops[0] = mop_vreg_def(dst);
            sr.ops[1] = mop_vreg_use(lhs); sr.ops[2] = mop_vreg_use(sh_m);
            emit(c, sr);
            return;
        }

        case PIR_OP_CMP_EQ:   case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LE_S:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GE_S:
        case PIR_OP_CMP_LT_U: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_U: case PIR_OP_CMP_GE_U: {
            MVReg dst = c->val_vreg[inst->results[0]];
            bool is_signed = (inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_LE_S ||
                              inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_GE_S);
            bool is_uns    = (inst->op == PIR_OP_CMP_LT_U || inst->op == PIR_OP_CMP_LE_U ||
                              inst->op == PIR_OP_CMP_GT_U || inst->op == PIR_OP_CMP_GE_U);
            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, is_signed, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, is_signed, loc);
            emit_cmp_rr(c, is_uns, lhs, rhs, loc);
            MCond cond = MCOND_EQ;
            if (inst->op == PIR_OP_CMP_EQ)        cond = MCOND_EQ;
            else if (inst->op == PIR_OP_CMP_NE)   cond = MCOND_NE;
            else if (inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_LT_U) cond = MCOND_LT;
            else if (inst->op == PIR_OP_CMP_LE_S || inst->op == PIR_OP_CMP_LE_U) cond = MCOND_LE;
            else if (inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_GT_U) cond = MCOND_GT;
            else                                  cond = MCOND_GE;
            emit_setcc(c, dst, cond, loc);
            return;
        }

        case PIR_OP_SELECT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg cond = emit_zext_sub(c, mat_op(c, &inst->operands[0], loc), 8u, loc);
            MVReg tv   = mat_op(c, &inst->operands[1], loc);
            MVReg fv   = mat_op(c, &inst->operands[2], loc);
            uint8_t w  = ppc_vreg_bytes(inst->ty);

            uint32_t tb = mfunc_new_block(c->mf, "sel_t");
            uint32_t fb = mfunc_new_block(c->mf, "sel_f");
            uint32_t mb = mfunc_new_block(c->mf, "sel_m");

            emit_cmp_ri(c, false, cond, 0, loc);
            MInst br = minst_make(MI_PPC_BC, 4u, loc);
            br.op_count = 2u; br.ops[0] = mop_cond(MCOND_NE); br.ops[1] = mop_block(tb);
            emit(c, br);
            MInst jf = minst_make(MI_PPC_B, 4u, loc);
            jf.op_count = 1u; jf.ops[0] = mop_block(fb);
            emit(c, jf);
            mfunc_add_edge(c->mf, c->cur_mblk, tb);
            mfunc_add_edge(c->mf, c->cur_mblk, fb);

            c->cur_mblk = tb;
            MInst ct = minst_make(MI_COPY, w, loc);
            ct.op_count = 2u; ct.ops[0] = mop_vreg_def(dst); ct.ops[1] = mop_vreg_use(tv);
            emit(c, ct);
            MInst jm1 = minst_make(MI_PPC_B, 4u, loc);
            jm1.op_count = 1u; jm1.ops[0] = mop_block(mb);
            emit(c, jm1);
            mfunc_add_edge(c->mf, tb, mb);

            c->cur_mblk = fb;
            MInst cf = minst_make(MI_COPY, w, loc);
            cf.op_count = 2u; cf.ops[0] = mop_vreg_def(dst); cf.ops[1] = mop_vreg_use(fv);
            emit(c, cf);
            MInst jm2 = minst_make(MI_PPC_B, 4u, loc);
            jm2.op_count = 1u; jm2.ops[0] = mop_block(mb);
            emit(c, jm2);
            mfunc_add_edge(c->mf, fb, mb);

            c->cur_mblk = mb;
            return;
        }

        case PIR_OP_ZEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            PirScalarType s_sc = inst->operands[0].type.u.scalar;
            MVReg norm = (s_sc == PIR_SCALAR_I8)  ? emit_zext_sub(c, src, 8u, loc)
                       : (s_sc == PIR_SCALAR_I16) ? emit_zext_sub(c, src, 16u, loc) : src;
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(norm);
            emit(c, cp);
            return;
        }

        case PIR_OP_SEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            PirScalarType s_sc = inst->operands[0].type.u.scalar;
            MVReg norm = (s_sc == PIR_SCALAR_I8)  ? emit_sext_sub(c, src, 8u, loc)
                       : (s_sc == PIR_SCALAR_I16) ? emit_sext_sub(c, src, 16u, loc) : src;
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(norm);
            emit(c, cp);
            return;
        }

        case PIR_OP_TRUNC:
        case PIR_OP_PTRTOINT:
        case PIR_OP_INTTOPTR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(src);
            emit(c, cp);
            return;
        }

        case PIR_OP_BITCAST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MRegClass drc = ppc_rclass(inst->ty);
            MRegClass src_rc = ppc_rclass(inst->operands[0].type);
            if (drc == src_rc) {
                MInst cp = minst_make(MI_COPY, ppc_vreg_bytes(inst->ty), loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(src);
                emit(c, cp);
            } else if (drc == MREG_CLASS_FP) {
                /* i32 -> f32 via 8B frame conversion slot */
                MVReg p0 = emit_conv_slot_addr(c, 0, loc);
                MInst st = minst_make(MI_PPC_ST, 4u, loc);
                st.op_count = 3u; st.ops[0] = mop_vreg_use(src); st.ops[1] = mop_vreg_use(p0); st.ops[2] = mop_imm(0);
                emit(c, st);
                MInst ld = minst_make(MI_PPC_FLD, 4u, loc);
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_vreg_use(p0); ld.ops[2] = mop_imm(0);
                emit(c, ld);
            } else {
                /* f32 -> i32 via 8B frame conversion slot */
                MVReg p0 = emit_conv_slot_addr(c, 0, loc);
                MInst st = minst_make(MI_PPC_FST, 4u, loc);
                st.op_count = 3u; st.ops[0] = mop_vreg_use(src); st.ops[1] = mop_vreg_use(p0); st.ops[2] = mop_imm(0);
                emit(c, st);
                MInst ld = minst_make(MI_PPC_LD, 4u, loc);
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_vreg_use(p0); ld.ops[2] = mop_imm(0);
                emit(c, ld);
            }
            return;
        }

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            uint8_t w = ppc_vreg_bytes(inst->ty);
            MOpcode fop = (inst->op == PIR_OP_FADD) ? MI_PPC_FADD
                        : (inst->op == PIR_OP_FSUB) ? MI_PPC_FSUB
                        : (inst->op == PIR_OP_FMUL) ? MI_PPC_FMUL : MI_PPC_FDIV;
            MInst fi = minst_make(fop, w, loc);
            fi.op_count = 3u; fi.ops[0] = mop_vreg_def(dst);
            fi.ops[1] = mop_vreg_use(lhs); fi.ops[2] = mop_vreg_use(rhs);
            emit(c, fi);
            return;
        }

        case PIR_OP_FNEG: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst fn = minst_make(MI_PPC_FNEG, ppc_vreg_bytes(inst->ty), loc);
            fn.op_count = 2u; fn.ops[0] = mop_vreg_def(dst); fn.ops[1] = mop_vreg_use(src);
            emit(c, fn);
            return;
        }

        case PIR_OP_FPEXT: {
            /* On PowerPC FPU, every f32 in an FPR is already represented as an IEEE double! */
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst cp = minst_make(MI_COPY, 8u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(src);
            emit(c, cp);
            return;
        }

        case PIR_OP_FPTRUNC: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst fr = minst_make(MI_PPC_FRSP, 4u, loc);
            fr.op_count = 2u; fr.ops[0] = mop_vreg_def(dst); fr.ops[1] = mop_vreg_use(src);
            emit(c, fr);
            return;
        }

        case PIR_OP_SITOFP:
        case PIR_OP_UITOFP: {
            MVReg dst = c->val_vreg[inst->results[0]];
            bool is_s = (inst->op == PIR_OP_SITOFP);
            MVReg src = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->operands[0].type, is_s, loc);
            uint8_t w = ppc_vreg_bytes(inst->ty);
            if (w == 8u) {
                emit_itof_f64(c, dst, src, is_s, loc);
            } else {
                MVReg d64 = new_fp(c, 8u);
                emit_itof_f64(c, d64, src, is_s, loc);
                MInst fr = minst_make(MI_PPC_FRSP, 4u, loc);
                fr.op_count = 2u; fr.ops[0] = mop_vreg_def(dst); fr.ops[1] = mop_vreg_use(d64);
                emit(c, fr);
            }
            return;
        }

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            bool is_s = (inst->op == PIR_OP_FPTOSI);
            bool is_sat = (inst->flags & PIR_FLAG_SAT) != 0u;
            bool is_trap = (inst->flags & PIR_FLAG_TRAP) != 0u;
            PirScalarType dsc = inst->ty.u.scalar;

            int64_t  imin = is_s ? ((dsc == PIR_SCALAR_I8) ? -128 : (dsc == PIR_SCALAR_I16) ? -32768 : INT32_MIN) : 0;
            uint64_t imax = is_s ? ((dsc == PIR_SCALAR_I8) ? 127u : (dsc == PIR_SCALAR_I16) ? 32767u : (uint64_t)INT32_MAX)
                                 : ((dsc == PIR_SCALAR_I8) ? 255u : (dsc == PIR_SCALAR_I16) ? 65535u : UINT32_MAX);
            double dmin = (double)imin;
            double dmax_excl = is_s
                ? ((dsc == PIR_SCALAR_I8) ? 128.0 : (dsc == PIR_SCALAR_I16) ? 32768.0 : 2147483648.0)
                : ((dsc == PIR_SCALAR_I8) ? 256.0 : (dsc == PIR_SCALAR_I16) ? 65536.0 : 4294967296.0);
            uint64_t bmin = 0u, bmax = 0u;
            memcpy(&bmin, &dmin, 8u);
            memcpy(&bmax, &dmax_excl, 8u);

            if (is_trap) {
                /* Trap on NaN, < dmin, or >= dmax_excl */
                MInst cn = minst_make(MI_PPC_FCMPU, 8u, loc);
                cn.op_count = 2u; cn.ops[0] = mop_vreg_use(src); cn.ops[1] = mop_vreg_use(src);
                emit(c, cn);
                emit_bc_trap(c, MCOND_VS, loc); /* unordered = NaN */

                MVReg fmin = new_fp(c, 8u);
                emit_imm_fp(c, fmin, bmin, 8u, loc);
                MInst cl = minst_make(MI_PPC_FCMPU, 8u, loc);
                cl.op_count = 2u; cl.ops[0] = mop_vreg_use(src); cl.ops[1] = mop_vreg_use(fmin);
                emit(c, cl);
                emit_bc_trap(c, MCOND_LT, loc);

                MVReg fmax = new_fp(c, 8u);
                emit_imm_fp(c, fmax, bmax, 8u, loc);
                MInst ch = minst_make(MI_PPC_FCMPU, 8u, loc);
                ch.op_count = 2u; ch.ops[0] = mop_vreg_use(src); ch.ops[1] = mop_vreg_use(fmax);
                emit(c, ch);
                emit_bc_trap(c, MCOND_GE, loc);
            }

            if (is_sat) {
                /* Saturating: NaN -> 0, < dmin -> imin, >= dmax_excl -> imax */
                uint32_t nan_b  = mfunc_new_block(c->mf, "fsat_nan");
                uint32_t chk_lo = mfunc_new_block(c->mf, "fsat_lo");
                uint32_t lo_b   = mfunc_new_block(c->mf, "fsat_min");
                uint32_t chk_hi = mfunc_new_block(c->mf, "fsat_hi");
                uint32_t hi_b   = mfunc_new_block(c->mf, "fsat_max");
                uint32_t conv_b = mfunc_new_block(c->mf, "fsat_cv");
                uint32_t done_b = mfunc_new_block(c->mf, "fsat_dn");

                MInst cn = minst_make(MI_PPC_FCMPU, 8u, loc);
                cn.op_count = 2u; cn.ops[0] = mop_vreg_use(src); cn.ops[1] = mop_vreg_use(src);
                emit(c, cn);
                MInst bn = minst_make(MI_PPC_BC, 4u, loc);
                bn.op_count = 2u; bn.ops[0] = mop_cond(MCOND_VS); bn.ops[1] = mop_block(nan_b);
                emit(c, bn);
                MInst jl = minst_make(MI_PPC_B, 4u, loc);
                jl.op_count = 1u; jl.ops[0] = mop_block(chk_lo);
                emit(c, jl);
                mfunc_add_edge(c->mf, c->cur_mblk, nan_b);
                mfunc_add_edge(c->mf, c->cur_mblk, chk_lo);

                c->cur_mblk = nan_b;
                emit_li(c, dst, 0, loc);
                MInst jd0 = minst_make(MI_PPC_B, 4u, loc);
                jd0.op_count = 1u; jd0.ops[0] = mop_block(done_b);
                emit(c, jd0);
                mfunc_add_edge(c->mf, nan_b, done_b);

                c->cur_mblk = chk_lo;
                MVReg fmin = new_fp(c, 8u);
                emit_imm_fp(c, fmin, bmin, 8u, loc);
                MInst cl = minst_make(MI_PPC_FCMPU, 8u, loc);
                cl.op_count = 2u; cl.ops[0] = mop_vreg_use(src); cl.ops[1] = mop_vreg_use(fmin);
                emit(c, cl);
                MInst bl = minst_make(MI_PPC_BC, 4u, loc);
                bl.op_count = 2u; bl.ops[0] = mop_cond(MCOND_LT); bl.ops[1] = mop_block(lo_b);
                emit(c, bl);
                MInst jh = minst_make(MI_PPC_B, 4u, loc);
                jh.op_count = 1u; jh.ops[0] = mop_block(chk_hi);
                emit(c, jh);
                mfunc_add_edge(c->mf, chk_lo, lo_b);
                mfunc_add_edge(c->mf, chk_lo, chk_hi);

                c->cur_mblk = lo_b;
                emit_li(c, dst, (int32_t)imin, loc);
                MInst jd1 = minst_make(MI_PPC_B, 4u, loc);
                jd1.op_count = 1u; jd1.ops[0] = mop_block(done_b);
                emit(c, jd1);
                mfunc_add_edge(c->mf, lo_b, done_b);

                c->cur_mblk = chk_hi;
                MVReg fmax = new_fp(c, 8u);
                emit_imm_fp(c, fmax, bmax, 8u, loc);
                MInst ch = minst_make(MI_PPC_FCMPU, 8u, loc);
                ch.op_count = 2u; ch.ops[0] = mop_vreg_use(src); ch.ops[1] = mop_vreg_use(fmax);
                emit(c, ch);
                MInst bh = minst_make(MI_PPC_BC, 4u, loc);
                bh.op_count = 2u; bh.ops[0] = mop_cond(MCOND_GE); bh.ops[1] = mop_block(hi_b);
                emit(c, bh);
                MInst jc = minst_make(MI_PPC_B, 4u, loc);
                jc.op_count = 1u; jc.ops[0] = mop_block(conv_b);
                emit(c, jc);
                mfunc_add_edge(c->mf, chk_hi, hi_b);
                mfunc_add_edge(c->mf, chk_hi, conv_b);

                c->cur_mblk = hi_b;
                emit_li(c, dst, (int32_t)(uint32_t)imax, loc);
                MInst jd2 = minst_make(MI_PPC_B, 4u, loc);
                jd2.op_count = 1u; jd2.ops[0] = mop_block(done_b);
                emit(c, jd2);
                mfunc_add_edge(c->mf, hi_b, done_b);

                c->cur_mblk = conv_b;
                if (!is_s && dsc == PIR_SCALAR_I32) {
                    /* Unsigned 32-bit in [0, 2^32): split at 2^31 */
                    uint32_t big_b = mfunc_new_block(c->mf, "fu_big");
                    uint32_t sml_b = mfunc_new_block(c->mf, "fu_sml");
                    MVReg f231 = new_fp(c, 8u);
                    emit_imm_fp(c, f231, UINT64_C(0x41e0000000000000), 8u, loc); /* 2147483648.0 */
                    MInst cu = minst_make(MI_PPC_FCMPU, 8u, loc);
                    cu.op_count = 2u; cu.ops[0] = mop_vreg_use(src); cu.ops[1] = mop_vreg_use(f231);
                    emit(c, cu);
                    MInst bu = minst_make(MI_PPC_BC, 4u, loc);
                    bu.op_count = 2u; bu.ops[0] = mop_cond(MCOND_GE); bu.ops[1] = mop_block(big_b);
                    emit(c, bu);
                    MInst js = minst_make(MI_PPC_B, 4u, loc);
                    js.op_count = 1u; js.ops[0] = mop_block(sml_b);
                    emit(c, js);
                    mfunc_add_edge(c->mf, conv_b, big_b);
                    mfunc_add_edge(c->mf, conv_b, sml_b);

                    c->cur_mblk = sml_b;
                    emit_fctiwz_to_gp(c, dst, src, loc);
                    MInst jds = minst_make(MI_PPC_B, 4u, loc);
                    jds.op_count = 1u; jds.ops[0] = mop_block(done_b);
                    emit(c, jds);
                    mfunc_add_edge(c->mf, sml_b, done_b);

                    c->cur_mblk = big_b;
                    MVReg fadj = new_fp(c, 8u);
                    MInst fsb = minst_make(MI_PPC_FSUB, 8u, loc);
                    fsb.op_count = 3u; fsb.ops[0] = mop_vreg_def(fadj);
                    fsb.ops[1] = mop_vreg_use(src); fsb.ops[2] = mop_vreg_use(f231);
                    emit(c, fsb);
                    MVReg tmp_i = new_gp(c);
                    emit_fctiwz_to_gp(c, tmp_i, fadj, loc);
                    MVReg hi_bit = new_gp(c);
                    emit_li(c, hi_bit, (int32_t)0x80000000u, loc);
                    MInst xr = minst_make(MI_PPC_XOR, 4u, loc);
                    xr.op_count = 3u; xr.ops[0] = mop_vreg_def(dst);
                    xr.ops[1] = mop_vreg_use(tmp_i); xr.ops[2] = mop_vreg_use(hi_bit);
                    emit(c, xr);
                    MInst jdb = minst_make(MI_PPC_B, 4u, loc);
                    jdb.op_count = 1u; jdb.ops[0] = mop_block(done_b);
                    emit(c, jdb);
                    mfunc_add_edge(c->mf, big_b, done_b);
                } else {
                    emit_fctiwz_to_gp(c, dst, src, loc);
                    MInst jdc = minst_make(MI_PPC_B, 4u, loc);
                    jdc.op_count = 1u; jdc.ops[0] = mop_block(done_b);
                    emit(c, jdc);
                    mfunc_add_edge(c->mf, conv_b, done_b);
                }
                c->cur_mblk = done_b;
                return;
            }

            /* Non-sat path (already trapped if is_trap) */
            if (!is_s && dsc == PIR_SCALAR_I32) {
                uint32_t big_b  = mfunc_new_block(c->mf, "fu_big");
                uint32_t sml_b  = mfunc_new_block(c->mf, "fu_sml");
                uint32_t done_b = mfunc_new_block(c->mf, "fu_dn");
                MVReg f231 = new_fp(c, 8u);
                emit_imm_fp(c, f231, UINT64_C(0x41e0000000000000), 8u, loc);
                MInst cu = minst_make(MI_PPC_FCMPU, 8u, loc);
                cu.op_count = 2u; cu.ops[0] = mop_vreg_use(src); cu.ops[1] = mop_vreg_use(f231);
                emit(c, cu);
                MInst bu = minst_make(MI_PPC_BC, 4u, loc);
                bu.op_count = 2u; bu.ops[0] = mop_cond(MCOND_GE); bu.ops[1] = mop_block(big_b);
                emit(c, bu);
                MInst js = minst_make(MI_PPC_B, 4u, loc);
                js.op_count = 1u; js.ops[0] = mop_block(sml_b);
                emit(c, js);
                mfunc_add_edge(c->mf, c->cur_mblk, big_b);
                mfunc_add_edge(c->mf, c->cur_mblk, sml_b);

                c->cur_mblk = sml_b;
                emit_fctiwz_to_gp(c, dst, src, loc);
                MInst j1 = minst_make(MI_PPC_B, 4u, loc);
                j1.op_count = 1u; j1.ops[0] = mop_block(done_b);
                emit(c, j1);
                mfunc_add_edge(c->mf, sml_b, done_b);

                c->cur_mblk = big_b;
                MVReg fadj = new_fp(c, 8u);
                MInst fsb = minst_make(MI_PPC_FSUB, 8u, loc);
                fsb.op_count = 3u; fsb.ops[0] = mop_vreg_def(fadj);
                fsb.ops[1] = mop_vreg_use(src); fsb.ops[2] = mop_vreg_use(f231);
                emit(c, fsb);
                MVReg tmp_i = new_gp(c);
                emit_fctiwz_to_gp(c, tmp_i, fadj, loc);
                MVReg hi_bit = new_gp(c);
                emit_li(c, hi_bit, (int32_t)0x80000000u, loc);
                MInst xr = minst_make(MI_PPC_XOR, 4u, loc);
                xr.op_count = 3u; xr.ops[0] = mop_vreg_def(dst);
                xr.ops[1] = mop_vreg_use(tmp_i); xr.ops[2] = mop_vreg_use(hi_bit);
                emit(c, xr);
                MInst j2 = minst_make(MI_PPC_B, 4u, loc);
                j2.op_count = 1u; j2.ops[0] = mop_block(done_b);
                emit(c, j2);
                mfunc_add_edge(c->mf, big_b, done_b);

                c->cur_mblk = done_b;
            } else {
                emit_fctiwz_to_gp(c, dst, src, loc);
            }
            return;
        }

        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE:
        case PIR_OP_FCMP_OLT: case PIR_OP_FCMP_OLE:
        case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE:
        case PIR_OP_FCMP_ULT: case PIR_OP_FCMP_ULE:
        case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MInst fc = minst_make(MI_PPC_FCMPU, ppc_vreg_bytes(inst->ty), loc);
            fc.op_count = 2u; fc.ops[0] = mop_vreg_use(lhs); fc.ops[1] = mop_vreg_use(rhs);
            emit(c, fc);

            /* CR0 after fcmpu: LT=bit0, GT=bit1, EQ=bit2, UN=bit3 (VS) */
            if (inst->op == PIR_OP_FCMP_OEQ)      emit_setcc(c, dst, MCOND_EQ, loc);
            else if (inst->op == PIR_OP_FCMP_OLT) emit_setcc(c, dst, MCOND_LT, loc);
            else if (inst->op == PIR_OP_FCMP_OGT) emit_setcc(c, dst, MCOND_GT, loc);
            else if (inst->op == PIR_OP_FCMP_UNE) emit_setcc(c, dst, MCOND_NE, loc);
            else if (inst->op == PIR_OP_FCMP_ULE) emit_setcc(c, dst, MCOND_LE, loc); /* !GT = LT|EQ|UN */
            else if (inst->op == PIR_OP_FCMP_UGE) emit_setcc(c, dst, MCOND_GE, loc); /* !LT = GT|EQ|UN */
            else {
                MCond c1 = MCOND_EQ, c2 = MCOND_EQ;
                if (inst->op == PIR_OP_FCMP_OLE)      { c1 = MCOND_LT; c2 = MCOND_EQ; }
                else if (inst->op == PIR_OP_FCMP_OGE) { c1 = MCOND_GT; c2 = MCOND_EQ; }
                else if (inst->op == PIR_OP_FCMP_ONE) { c1 = MCOND_LT; c2 = MCOND_GT; }
                else if (inst->op == PIR_OP_FCMP_UEQ) { c1 = MCOND_EQ; c2 = MCOND_VS; }
                else if (inst->op == PIR_OP_FCMP_ULT) { c1 = MCOND_LT; c2 = MCOND_VS; }
                else                                  { c1 = MCOND_GT; c2 = MCOND_VS; }
                MVReg t1 = new_gp(c), t2 = new_gp(c);
                emit_setcc(c, t1, c1, loc);
                emit_setcc(c, t2, c2, loc);
                MInst oi = minst_make(MI_PPC_OR, 4u, loc);
                oi.op_count = 3u; oi.ops[0] = mop_vreg_def(dst);
                oi.ops[1] = mop_vreg_use(t1); oi.ops[2] = mop_vreg_use(t2);
                emit(c, oi);
            }
            return;
        }

        case PIR_OP_ALLOCA_PTR: {
            PirValueId res = inst->results[0];
            MVReg dst = c->val_vreg[res];
            uint32_t sid = c->alloca_slot[res];
            MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
            fa.op_count = 3u; fa.ops[0] = mop_vreg_def(dst);
            fa.ops[1] = mop_slot(sid); fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return;
        }

        case PIR_OP_ALLOCA_DYN: {
            /* SysV PPC32 dynamic alloca: keep backchain word at 0(sp) valid & 16B-aligned!
             * Also leave 8 bytes of header at sp+0..sp+7, return sp + 8? Even simpler:
             * round sz up to 16B (min 16B), allocate (sz + 16), copy backchain *(sp) -> *(new_sp),
             * return new_sp + 16 (leaves full 16B header/outarg margin below returned buffer!). */
            c->mf->has_alloca_dyn = true;
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg sz  = mat_op(c, &inst->operands[0], loc);
            uint32_t al = inst->align > 16u ? inst->align : 16u;
            MVReg bc = new_gp(c), cur_sp = new_gp(c), sz_pad = new_gp(c), nsp = new_gp(c);

            MInst gsp = minst_make(MI_PPC_MOV_SP, 4u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(cur_sp);
            emit(c, gsp);

            MInst lbc = minst_make(MI_PPC_LD, 4u, loc);
            lbc.op_count = 3u; lbc.ops[0] = mop_vreg_def(bc);
            lbc.ops[1] = mop_vreg_use(cur_sp); lbc.ops[2] = mop_imm(0);
            emit(c, lbc);

            /* sz_pad = (sz + 31) & ~15 (adds 16B header room) */
            MInst a31 = minst_make(MI_PPC_ADDI, 4u, loc);
            a31.op_count = 3u; a31.ops[0] = mop_vreg_def(sz_pad);
            a31.ops[1] = mop_vreg_use(sz); a31.ops[2] = mop_imm(31);
            emit(c, a31);

            MVReg m16 = new_gp(c), sz_al = new_gp(c);
            emit_li(c, m16, -16, loc);
            MInst an = minst_make(MI_PPC_AND, 4u, loc);
            an.op_count = 3u; an.ops[0] = mop_vreg_def(sz_al);
            an.ops[1] = mop_vreg_use(sz_pad); an.ops[2] = mop_vreg_use(m16);
            emit(c, an);

            MInst sb = minst_make(MI_PPC_SUB, 4u, loc);
            sb.op_count = 3u; sb.ops[0] = mop_vreg_def(nsp);
            sb.ops[1] = mop_vreg_use(cur_sp); sb.ops[2] = mop_vreg_use(sz_al);
            emit(c, sb);

            if (al > 16u) {
                MVReg mal = new_gp(c), nsp2 = new_gp(c);
                emit_li(c, mal, -(int32_t)al, loc);
                MInst aa = minst_make(MI_PPC_AND, 4u, loc);
                aa.op_count = 3u; aa.ops[0] = mop_vreg_def(nsp2);
                aa.ops[1] = mop_vreg_use(nsp); aa.ops[2] = mop_vreg_use(mal);
                emit(c, aa);
                nsp = nsp2;
            }

            /* Store backchain at 0(nsp), update sp = nsp, return nsp + 16 */
            MInst sbc = minst_make(MI_PPC_ST, 4u, loc);
            sbc.op_count = 3u; sbc.ops[0] = mop_vreg_use(bc);
            sbc.ops[1] = mop_vreg_use(nsp); sbc.ops[2] = mop_imm(0);
            emit(c, sbc);

            MInst ssp = minst_make(MI_PPC_MOV_SP, 4u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(nsp);
            emit(c, ssp);

            MInst retp = minst_make(MI_PPC_ADDI, 4u, loc);
            retp.op_count = 3u; retp.ops[0] = mop_vreg_def(dst);
            retp.ops[1] = mop_vreg_use(nsp); retp.ops[2] = mop_imm(16);
            emit(c, retp);
            return;
        }

        case PIR_OP_STACKSAVE: {
            c->mf->has_alloca_dyn = true;
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst gsp = minst_make(MI_PPC_MOV_SP, 4u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(dst);
            emit(c, gsp);
            return;
        }

        case PIR_OP_STACKRESTORE: {
            c->mf->has_alloca_dyn = true;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst ssp = minst_make(MI_PPC_MOV_SP, 4u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(src);
            emit(c, ssp);
            return;
        }

        case PIR_OP_PTR_ADD: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg base = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                int32_t off = (int32_t)eval_imm_u32(&inst->operands[1]);
                if (off == 0) {
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(base);
                    emit(c, cp);
                    return;
                }
                if (fits_simm16(off)) {
                    MInst ai = minst_make(MI_PPC_ADDI, 4u, loc);
                    ai.op_count = 3u; ai.ops[0] = mop_vreg_def(dst);
                    ai.ops[1] = mop_vreg_use(base); ai.ops[2] = mop_imm(off);
                    emit(c, ai);
                    return;
                }
            }
            MVReg idx = mat_op(c, &inst->operands[1], loc);
            MInst ad = minst_make(MI_PPC_ADD, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
            ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(idx);
            emit(c, ad);
            return;
        }

        case PIR_OP_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = ppc_mem_bytes(inst->ty);
            bool fp = (ppc_rclass(inst->ty) == MREG_CLASS_FP);
            MInst ld = minst_make(fp ? MI_PPC_FLD : MI_PPC_LD, w, loc);
            ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
            emit(c, ld);
            return;
        }

        case PIR_OP_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = ppc_mem_bytes(inst->ty);
            bool fp = (ppc_rclass(inst->ty) == MREG_CLASS_FP);
            MInst st = minst_make(fp ? MI_PPC_FST : MI_PPC_ST, w, loc);
            st.op_count = 3u; st.ops[0] = mop_vreg_use(val);
            st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
            emit(c, st);
            return;
        }

        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET: {
            c->mf->has_calls = true;
            MVReg a0 = mat_op(c, &inst->operands[0], loc);
            MVReg a1 = mat_op(c, &inst->operands[1], loc);
            MVReg a2 = mat_op(c, &inst->operands[2], loc);
            if (inst->op == PIR_OP_MEMSET) a1 = emit_zext_sub(c, a1, 8u, loc);

            MVReg f0 = new_gp(c), f1 = new_gp(c), f2 = new_gp(c);
            c->mf->vregs.data[f0].fixed_preg = PPC_R3;
            c->mf->vregs.data[f1].fixed_preg = PPC_R4;
            c->mf->vregs.data[f2].fixed_preg = PPC_R5;

            MInst c0 = minst_make(MI_COPY, 4u, loc);
            c0.op_count = 2u; c0.ops[0] = mop_vreg_def(f0); c0.ops[1] = mop_vreg_use(a0); emit(c, c0);
            MInst c1 = minst_make(MI_COPY, 4u, loc);
            c1.op_count = 2u; c1.ops[0] = mop_vreg_def(f1); c1.ops[1] = mop_vreg_use(a1); emit(c, c1);
            MInst c2 = minst_make(MI_COPY, 4u, loc);
            c2.op_count = 2u; c2.ops[0] = mop_vreg_def(f2); c2.ops[1] = mop_vreg_use(a2); emit(c, c2);

            const char *sym = (inst->op == PIR_OP_MEMCPY)  ? "memcpy"
                            : (inst->op == PIR_OP_MEMMOVE) ? "memmove" : "memset";
            MInst cl = minst_make(MI_PPC_CALL, 4u, loc);
            cl.is_call = true; cl.op_count = 1u; cl.ops[0] = mop_sym(sym, 0, true, false, false);
            emit(c, cl);
            return;
        }

        case PIR_OP_FENCE: {
            MInst f = minst_make(MI_PPC_SYNC, 4u, loc);
            f.aux_id = (uint32_t)inst->order;
            emit(c, f);
            return;
        }

        case PIR_OP_ATOMIC_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = ppc_mem_bytes(inst->ty);
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst f0 = minst_make(MI_PPC_SYNC, 4u, loc);
                f0.aux_id = (uint32_t)PIR_ORDER_SEQ_CST;
                emit(c, f0);
            }
            MInst ld = minst_make(MI_PPC_LD, w, loc);
            ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
            emit(c, ld);
            if (inst->order == PIR_ORDER_ACQUIRE || inst->order == PIR_ORDER_SEQ_CST) {
                MInst f1 = minst_make(MI_PPC_SYNC, 4u, loc);
                f1.aux_id = (uint32_t)PIR_ORDER_ACQUIRE;
                emit(c, f1);
            }
            return;
        }

        case PIR_OP_ATOMIC_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = ppc_mem_bytes(inst->ty);
            if (inst->order == PIR_ORDER_RELEASE || inst->order == PIR_ORDER_SEQ_CST) {
                MInst f0 = minst_make(MI_PPC_SYNC, 4u, loc);
                f0.aux_id = (uint32_t)PIR_ORDER_RELEASE;
                emit(c, f0);
            }
            MInst st = minst_make(MI_PPC_ST, w, loc);
            st.op_count = 3u; st.ops[0] = mop_vreg_use(val);
            st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
            emit(c, st);
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst f1 = minst_make(MI_PPC_SYNC, 4u, loc);
                f1.aux_id = (uint32_t)PIR_ORDER_SEQ_CST;
                emit(c, f1);
            }
            return;
        }

        case PIR_OP_ATOMIC_RMW: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);
            uint8_t w = ppc_mem_bytes(inst->ty);
            /* Pin to r3..r5 so r11/r12/r0 scratch regs in the LL/SC loop never collide. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = PPC_R3;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = PPC_R4;
            MVReg p_val = new_gp(c); c->mf->vregs.data[p_val].fixed_preg = PPC_R5;

            MInst cp1 = minst_make(MI_COPY, 4u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr); emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, 4u, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_val); cp2.ops[1] = mop_vreg_use(val); emit(c, cp2);

            MInst rmw = minst_make(MI_PPC_ATOMIC_RMW, w, loc);
            rmw.aux_id   = (uint32_t)inst->rmw_op;
            rmw.aux_id2  = (uint32_t)inst->order;
            rmw.op_count = 3u;
            rmw.ops[0]   = mop_vreg_def_ec(p_old);
            rmw.ops[1]   = mop_vreg_use(p_ptr);
            rmw.ops[2]   = mop_vreg_use(p_val);
            emit(c, rmw);

            MInst co = minst_make(MI_COPY, 4u, loc);
            co.op_count = 2u; co.ops[0] = mop_vreg_def(dst); co.ops[1] = mop_vreg_use(p_old); emit(c, co);
            return;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            MVReg old_v = c->val_vreg[inst->results[0]];
            MVReg ok_v  = c->val_vreg[inst->results[1]];
            MVReg ptr   = mat_op(c, &inst->operands[0], loc);
            MVReg exp_v = mat_op(c, &inst->operands[1], loc);
            MVReg des_v = mat_op(c, &inst->operands[2], loc);
            uint8_t w   = ppc_mem_bytes(inst->ty);
            /* Pin all 5 operands to r3..r7 so high pressure never exceeds 3 spill scratches. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = PPC_R3;
            MVReg p_ok  = new_gp(c); c->mf->vregs.data[p_ok].fixed_preg  = PPC_R4;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = PPC_R5;
            MVReg p_exp = new_gp(c); c->mf->vregs.data[p_exp].fixed_preg = PPC_R6;
            MVReg p_des = new_gp(c); c->mf->vregs.data[p_des].fixed_preg = PPC_R7;

            MInst cp1 = minst_make(MI_COPY, 4u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr); emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, 4u, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_exp); cp2.ops[1] = mop_vreg_use(exp_v); emit(c, cp2);
            MInst cp3 = minst_make(MI_COPY, 4u, loc);
            cp3.op_count = 2u; cp3.ops[0] = mop_vreg_def(p_des); cp3.ops[1] = mop_vreg_use(des_v); emit(c, cp3);

            MInst cas = minst_make(MI_PPC_ATOMIC_CAS, w, loc);
            cas.aux_id   = (uint32_t)inst->order;
            cas.aux_id2  = (uint32_t)inst->fail_order;
            cas.op_count = 5u;
            cas.ops[0]   = mop_vreg_def_ec(p_old);
            cas.ops[1]   = mop_vreg_def_ec(p_ok);
            cas.ops[2]   = mop_vreg_use(p_ptr);
            cas.ops[3]   = mop_vreg_use(p_exp);
            cas.ops[4]   = mop_vreg_use(p_des);
            emit(c, cas);

            MInst co1 = minst_make(MI_COPY, 4u, loc);
            co1.op_count = 2u; co1.ops[0] = mop_vreg_def(old_v); co1.ops[1] = mop_vreg_use(p_old); emit(c, co1);
            MInst co2 = minst_make(MI_COPY, 4u, loc);
            co2.op_count = 2u; co2.ops[0] = mop_vreg_def(ok_v); co2.ops[1] = mop_vreg_use(p_ok); emit(c, co2);
            return;
        }

        case PIR_OP_VA_START: {
            MVReg ap = mat_op(c, &inst->operands[0], loc);
            MInst vs = minst_make(MI_VA_START, 4u, loc);
            vs.op_count = 1u; vs.ops[0] = mop_vreg_use(ap);
            emit(c, vs);
            return;
        }

        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
            isel_call(c, inst, loc);
            return;

        case PIR_OP_JMP: {
            emit_branch_pcopy(c, &inst->targets[0], loc);
            uint32_t dst_mb = c->blk_map[inst->targets[0].target];
            MInst j = minst_make(MI_PPC_B, 4u, loc);
            j.op_count = 1u; j.ops[0] = mop_block(dst_mb);
            emit(c, j);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_mb);
            return;
        }

        case PIR_OP_BR: {
            MVReg cond = emit_zext_sub(c, mat_op(c, &inst->operands[0], loc), 8u, loc);
            uint32_t t_mb = resolve_edge_target(c, &inst->targets[0], loc);
            uint32_t f_mb = resolve_edge_target(c, &inst->targets[1], loc);

            emit_cmp_ri(c, false, cond, 0, loc);
            MInst br = minst_make(MI_PPC_BC, 4u, loc);
            br.op_count = 2u; br.ops[0] = mop_cond(MCOND_NE); br.ops[1] = mop_block(t_mb);
            emit(c, br);

            MInst jf = minst_make(MI_PPC_B, 4u, loc);
            jf.op_count = 1u; jf.ops[0] = mop_block(f_mb);
            emit(c, jf);

            mfunc_add_edge(c->mf, c->cur_mblk, t_mb);
            mfunc_add_edge(c->mf, c->cur_mblk, f_mb);
            return;
        }

        case PIR_OP_SWITCH: {
            MVReg val = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, true, loc);
            uint32_t def_b = resolve_edge_target(c, &inst->targets[0], loc);
            uint32_t nc = inst->switch_case_count;

            bool use_tbl = false;
            int64_t min_k = 0, max_k = 0;
            if (nc >= 4u) {
                for (uint32_t k = 0u; k < nc; ++k) {
                    uint32_t raw = (uint32_t)(inst->switch_cases[k].is_negative
                                              ? (UINT64_C(0) - inst->switch_cases[k].case_lo)
                                              : inst->switch_cases[k].case_lo);
                    int64_t sv = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? (int64_t)(int8_t)raw
                               : (inst->ty.u.scalar == PIR_SCALAR_I16) ? (int64_t)(int16_t)raw
                                                                       : (int64_t)(int32_t)raw;
                    if (k == 0u || sv < min_k) min_k = sv;
                    if (k == 0u || sv > max_k) max_k = sv;
                }
                if (max_k >= min_k && (uint64_t)(max_k - min_k) < 128u) {
                    use_tbl = true;
                }
            }

            if (use_tbl) {
                uint32_t span = (uint32_t)(max_k - min_k) + 1u;
                uint32_t *targs = (uint32_t *)malloc(span * sizeof(uint32_t));
                if (!targs) abort();
                for (uint32_t i = 0u; i < span; ++i) targs[i] = def_b;
                for (uint32_t k = 0u; k < nc; ++k) {
                    uint32_t raw = (uint32_t)(inst->switch_cases[k].is_negative
                                              ? (UINT64_C(0) - inst->switch_cases[k].case_lo)
                                              : inst->switch_cases[k].case_lo);
                    int64_t sv = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? (int64_t)(int8_t)raw
                               : (inst->ty.u.scalar == PIR_SCALAR_I16) ? (int64_t)(int16_t)raw
                                                                       : (int64_t)(int32_t)raw;
                    uint32_t tb = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                    targs[(uint32_t)(sv - min_k)] = tb;
                }
                uint32_t jt_id = mfunc_add_jtable(c->mf, min_k, span, targs, def_b);
                MVReg p_val = new_gp(c);
                c->mf->vregs.data[p_val].fixed_preg = PPC_R3;
                MInst cp = minst_make(MI_COPY, 4u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(p_val); cp.ops[1] = mop_vreg_use(val);
                emit(c, cp);
                MInst st = minst_make(MI_PPC_SWITCH_TBL, 4u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(p_val);
                st.ops[1] = mop_jtable(jt_id);
                st.ops[2] = mop_block(def_b);
                emit(c, st);
                mfunc_add_edge(c->mf, c->cur_mblk, def_b);
                for (uint32_t i = 0u; i < span; ++i) {
                    mfunc_add_edge(c->mf, c->cur_mblk, targs[i]);
                }
                free(targs);
                return;
            }

            for (uint32_t k = 0u; k < nc; ++k) {
                uint32_t raw = (uint32_t)(inst->switch_cases[k].is_negative
                                          ? (UINT64_C(0) - inst->switch_cases[k].case_lo)
                                          : inst->switch_cases[k].case_lo);
                int32_t sv = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? (int32_t)(int8_t)raw
                           : (inst->ty.u.scalar == PIR_SCALAR_I16) ? (int32_t)(int16_t)raw
                                                                   : (int32_t)raw;
                if (fits_simm16(sv)) {
                    emit_cmp_ri(c, false, val, sv, loc);
                } else {
                    MVReg kv = new_gp(c);
                    emit_li(c, kv, sv, loc);
                    emit_cmp_rr(c, false, val, kv, loc);
                }
                uint32_t tb = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                MInst br = minst_make(MI_PPC_BC, 4u, loc);
                br.op_count = 2u; br.ops[0] = mop_cond(MCOND_EQ); br.ops[1] = mop_block(tb);
                emit(c, br);
                mfunc_add_edge(c->mf, c->cur_mblk, tb);
            }
            MInst jd = minst_make(MI_PPC_B, 4u, loc);
            jd.op_count = 1u; jd.ops[0] = mop_block(def_b);
            emit(c, jd);
            mfunc_add_edge(c->mf, c->cur_mblk, def_b);
            return;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && c->multi_ret_vreg != MVREG_NONE) {
                uint32_t nregs = c->fn->cabi_ret.reg_count;
                uint64_t agg_sz = c->fn->cabi_ret.agg_size > 0u ? c->fn->cabi_ret.agg_size : ((uint64_t)nregs * 4u);
                MVReg vals[2] = { MVREG_NONE, MVREG_NONE };
                for (uint32_t k = 0u; k < nregs && k < 2u; ++k) {
                    uint64_t off = (uint64_t)k * 4u;
                    uint64_t rem = (agg_sz > off) ? (agg_sz - off) : 0u;
                    vals[k] = new_gp(c);
                    if (rem == 0u) {
                        emit_li(c, vals[k], 0, loc);
                    } else if (rem < 4u) {
                        /* Big-endian partial trailing word: byte 0 at MSB (bits 24..31), etc. */
                        MVReg acc = new_gp(c);
                        emit_li(c, acc, 0, loc);
                        for (uint64_t b = 0u; b < rem; ++b) {
                            MVReg bv = new_gp(c);
                            MInst lb = minst_make(MI_PPC_LD, 1u, loc);
                            lb.op_count = 3u; lb.ops[0] = mop_vreg_def(bv);
                            lb.ops[1] = mop_vreg_use(c->multi_ret_vreg); lb.ops[2] = mop_imm((int64_t)(off + b));
                            emit(c, lb);
                            uint32_t sh = (uint32_t)((3u - b) * 8u);
                            if (sh > 0u) {
                                MVReg bs = new_gp(c);
                                MInst sl = minst_make(MI_PPC_SLWI, 4u, loc);
                                sl.op_count = 3u; sl.ops[0] = mop_vreg_def(bs);
                                sl.ops[1] = mop_vreg_use(bv); sl.ops[2] = mop_imm((int64_t)sh);
                                emit(c, sl);
                                bv = bs;
                            }
                            MVReg nx = (b + 1u == rem) ? vals[k] : new_gp(c);
                            MInst oi = minst_make(MI_PPC_OR, 4u, loc);
                            oi.op_count = 3u; oi.ops[0] = mop_vreg_def(nx);
                            oi.ops[1] = mop_vreg_use(acc); oi.ops[2] = mop_vreg_use(bv);
                            emit(c, oi);
                            acc = nx;
                        }
                    } else {
                        MInst ld = minst_make(MI_PPC_LD, 4u, loc);
                        ld.op_count = 3u; ld.ops[0] = mop_vreg_def(vals[k]);
                        ld.ops[1] = mop_vreg_use(c->multi_ret_vreg); ld.ops[2] = mop_imm((int64_t)off);
                        emit(c, ld);
                    }
                }
                for (uint32_t k = 0u; k < nregs && k < 2u; ++k) {
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_preg_def(PPC_GP(3u + k));
                    cp.ops[1] = mop_vreg_use(vals[k]);
                    emit(c, cp);
                }
            } else if (inst->operand_count == 1u) {
                MVReg rv = mat_op(c, &inst->operands[0], loc);
                PirType rt = c->fn->ret_type;
                if (ppc_rclass(rt) == MREG_CLASS_GP && rt.kind == PIR_TYPE_SCALAR &&
                    (rt.u.scalar == PIR_SCALAR_I8 || rt.u.scalar == PIR_SCALAR_I16)) {
                    bool sx = (c->fn->attrs & PIR_FATTR_RET_SIGNEXT) != 0u;
                    rv = normalize_int(c, rv, rt, sx, loc);
                }
                MPReg rpreg = (ppc_rclass(rt) == MREG_CLASS_FP) ? PPC_F1 : PPC_R3;
                MInst cp = minst_make(MI_COPY, ppc_vreg_bytes(rt), loc);
                cp.op_count = 2u; cp.ops[0] = mop_preg_def(rpreg); cp.ops[1] = mop_vreg_use(rv);
                emit(c, cp);
            }
            MInst rt = minst_make(MI_PPC_RET, 4u, loc);
            emit(c, rt);
            return;
        }

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE: {
            MInst tr = minst_make(MI_PPC_TRAP, 4u, loc);
            emit(c, tr);
            return;
        }

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
        case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
            pir_diag_error(c->diag, inst->src_line, 0u,
                           "PPC32 ISel: unlowered or unsupported PIR opcode %u", (unsigned)inst->op);
            c->ok = false;
            return;
    }
}

/* =========================================================================
 * Entry Point: Function-Level Instruction Selection
 * ========================================================================= */

bool pir_ppc32_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                         const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);
    out_mf->is_variadic = fn->is_variadic;

    PpcISelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod    = mod;
    ctx.fn     = fn;
    ctx.target = target;
    ctx.mf     = out_mf;
    ctx.diag   = diag;
    ctx.ok     = true;
    ctx.multi_ret_vreg = MVREG_NONE;

    uint32_t nvals = fn->values.len;
    uint32_t nblks = fn->blocks.len;
    ctx.val_vreg    = (MVReg *)malloc((nvals + 1u) * sizeof(MVReg));
    ctx.alloca_slot = (uint32_t *)malloc((nvals + 1u) * sizeof(uint32_t));
    ctx.blk_map     = (uint32_t *)malloc((nblks + 1u) * sizeof(uint32_t));
    if (!ctx.val_vreg || !ctx.alloca_slot || !ctx.blk_map) abort();

    for (uint32_t v = 0u; v < nvals; ++v) {
        PirType vt = fn->values.data[v].type;
        if (vt.kind == PIR_TYPE_SCALAR && vt.u.scalar == PIR_SCALAR_VOID) {
            ctx.val_vreg[v] = MVREG_NONE;
        } else {
            ctx.val_vreg[v] = mfunc_new_vreg(out_mf, ppc_rclass(vt), ppc_vreg_bytes(vt));
        }
        ctx.alloca_slot[v] = UINT32_MAX;
    }

    /* Pre-assign stack slots for all static alloca_ptr instructions. */
    for (uint32_t b = 0u; b < nblks; ++b) {
        const PirBlock *blk = &fn->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_ALLOCA_PTR) {
                uint64_t sz = ins->imm_u64 > 0u ? ins->imm_u64 : 1u;
                uint32_t al = ins->align > 0u ? ins->align : 1u;
                ctx.alloca_slot[ins->results[0]] = mfunc_add_slot(out_mf, sz, al, false);
            }
        }
    }

    /* Compute RPO block order so fallthrough layout is natural. */
    PirCfg cfg;
    pir_cfg_build(&cfg, fn);
    for (uint32_t r = 0u; r < cfg.rpo_len; ++r) {
        PirBlockId pb = cfg.rpo[r];
        ctx.blk_map[pb] = mfunc_new_block(out_mf, fn->blocks.data[pb].label);
    }
    for (uint32_t b = 0u; b < nblks; ++b) {
        bool in_rpo = false;
        for (uint32_t r = 0u; r < cfg.rpo_len; ++r) {
            if (cfg.rpo[r] == b) { in_rpo = true; break; }
        }
        if (!in_rpo) ctx.blk_map[b] = mfunc_new_block(out_mf, fn->blocks.data[b].label);
    }

    /* Lower incoming parameters in block 0 (SysV PPC32 SVR4 ABI). */
    ctx.cur_mblk = ctx.blk_map[0];
    uint32_t gp_i = 0u, fp_i = 0u;
    uint64_t stk_off = 0u;
    uint32_t first_p = 0u;

    if (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && fn->param_count > 0u) {
        uint64_t sz = fn->cabi_ret.agg_size > 0u ? fn->cabi_ret.agg_size : 8u;
        uint32_t al = fn->cabi_ret.agg_align > 0u ? fn->cabi_ret.agg_align : 4u;
        uint32_t sid = mfunc_add_slot(out_mf, sz, al, false);
        MVReg rv = ctx.val_vreg[fn->params[0].val_id];
        MInst fa = minst_make(MI_FRAME_ADDR, 4u, 0u);
        fa.op_count = 3u; fa.ops[0] = mop_vreg_def(rv);
        fa.ops[1] = mop_slot(sid); fa.ops[2] = mop_imm(0);
        emit(&ctx, fa);
        ctx.multi_ret_vreg = rv;
        first_p = 1u;
    }

    for (uint32_t i = first_p; i < fn->param_count; ++i) {
        const PirParamDecl *pd = &fn->params[i];
        MVReg dst = (pd->val_id != PIR_INVALID_ID) ? ctx.val_vreg[pd->val_id] : MVREG_NONE;
        bool fp = (ppc_rclass(pd->type) == MREG_CLASS_FP);
        uint8_t w = ppc_vreg_bytes(pd->type);

        if (fp) {
            if (!pd->abi_force_stack && fp_i < 8u) {
                if (dst != MVREG_NONE) {
                    MInst cp = minst_make(MI_COPY, w, 0u);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst);
                    cp.ops[1] = mop_preg_use(PPC_FP(1u + fp_i));
                    emit(&ctx, cp);
                }
                fp_i++;
            } else {
                uint64_t al = (w >= 8u || pd->abi_stack_align >= 8u) ? 8u : 4u;
                stk_off = (stk_off + al - 1u) & ~(al - 1u);
                if (dst != MVREG_NONE) {
                    MInst li = minst_make(MI_LOAD_INARG, w, 0u);
                    li.op_count = 2u; li.ops[0] = mop_vreg_def(dst);
                    li.ops[1] = mop_imm((int64_t)stk_off);
                    emit(&ctx, li);
                }
                stk_off += w;
            }
        } else {
            bool need_al8 = (pd->abi_stack_align >= 8u);
            if (!pd->abi_force_stack) {
                if (need_al8 && (gp_i & 1u) != 0u && gp_i < 8u) {
                    gp_i++;
                }
                if (need_al8 && gp_i + 2u > 8u) {
                    gp_i = 8u;
                }
            }
            if (!pd->abi_force_stack && gp_i < 8u) {
                if (dst != MVREG_NONE) {
                    MInst cp = minst_make(MI_COPY, 4u, 0u);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst);
                    cp.ops[1] = mop_preg_use(PPC_GP(3u + gp_i));
                    emit(&ctx, cp);
                }
                gp_i++;
            } else {
                uint64_t al = need_al8 ? 8u : 4u;
                stk_off = (stk_off + al - 1u) & ~(al - 1u);
                if (dst != MVREG_NONE) {
                    MInst li = minst_make(MI_LOAD_INARG, 4u, 0u);
                    li.op_count = 2u; li.ops[0] = mop_vreg_def(dst);
                    li.ops[1] = mop_imm((int64_t)stk_off);
                    emit(&ctx, li);
                }
                stk_off += 4u;
            }
        }
    }

    out_mf->va_fixed_gp          = gp_i;
    out_mf->va_fixed_fp          = fp_i;
    out_mf->va_fixed_stack_bytes = stk_off;

    /* Select instructions for every reachable block in RPO order. */
    for (uint32_t r = 0u; r < cfg.rpo_len && ctx.ok; ++r) {
        PirBlockId pb = cfg.rpo[r];
        ctx.cur_mblk = ctx.blk_map[pb];
        const PirBlock *blk = &fn->blocks.data[pb];
        for (uint32_t j = 0u; j < blk->insts.len && ctx.ok; ++j) {
            isel_inst(&ctx, &fn->insts.data[blk->insts.data[j]]);
        }
    }

    pir_cfg_destroy(&cfg);
    free(ctx.val_vreg);
    free(ctx.alloca_slot);
    free(ctx.blk_map);
    return ctx.ok;
}
