/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RISC-V 32-bit (RV32IMAFD / ILP32D) Instruction Selection (LIR -> MIR).
 *
 * Preconditions enforced by middle-end lowering & LIR verifier:
 *   - ptr_bytes == 4, max_int_bits == 32, max_simd_bits == 0
 *   - No i128, no i64, no vectors, no aggregates, no HIR ops
 *   - Flagless optional ops (ovf, add_carry, sub_borrow, bitops) already expanded
 *   - Critical edges already split (pir_opt_edge_split)
 */

#include "backend/backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "lower/lower.h"
#include "opt/opt.h"

/* =========================================================================
 * Type & Register Helpers
 * ========================================================================= */

static MRegClass rv32_rclass(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR &&
        (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64)) {
        return MREG_CLASS_FP;
    }
    return MREG_CLASS_GP;
}

static uint8_t rv32_vreg_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR && ty.u.scalar == PIR_SCALAR_F64) {
        return 8u;
    }
    return 4u;
}

static uint8_t rv32_mem_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:  return 1u;
            case PIR_SCALAR_I16: return 2u;
            case PIR_SCALAR_I32:
            case PIR_SCALAR_F32:
            case PIR_SCALAR_PTR: return 4u;
            case PIR_SCALAR_F64: return 8u;
            case PIR_SCALAR_VOID:
            case PIR_SCALAR_I64:
            case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST: return 0u;
        }
    }
    return 4u;
}

static uint32_t eval_imm_u32(const PirOperand *op) {
    uint64_t v = op->u.imm_int.lo;
    if (op->u.imm_int.is_negative) {
        v = UINT64_C(0) - v;
    }
    return (uint32_t)(v & 0xFFFFFFFFu);
}

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool fits_simm12(int64_t v) {
    return v >= -2048 && v <= 2047;
}

/* =========================================================================
 * ISel Context
 * ========================================================================= */

typedef struct {
    const PirModule     *mod;
    const PirFunction   *fn;
    const PirTargetSpec *target;
    MFunc               *mf;
    PirDiag             *diag;

    MVReg               *val_vreg;
    uint32_t            *pir_to_mblk;
    uint32_t             cur_mblk;
    uint32_t             multi_ret_slot;
    uint32_t             f64_xfer_slot; /* Reusable 8B slot for f64 <-> (aN, aN+1) */
} Rv32ISelCtx;

static MBlock *cur_blk(Rv32ISelCtx *c) {
    return &c->mf->blocks.data[c->cur_mblk];
}

static void emit(Rv32ISelCtx *c, MInst ins) {
    mblock_push(cur_blk(c), ins);
}

static MVReg new_gp(Rv32ISelCtx *c) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, 4u);
}

static MVReg new_fp(Rv32ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_FP, bytes);
}

static MVReg new_vreg_for_type(Rv32ISelCtx *c, PirType ty) {
    return mfunc_new_vreg(c->mf, rv32_rclass(ty), rv32_vreg_bytes(ty));
}

static uint32_t get_f64_xfer_slot(Rv32ISelCtx *c) {
    if (c->f64_xfer_slot == UINT32_MAX) {
        c->f64_xfer_slot = mfunc_add_slot(c->mf, 8u, 8u, false);
    }
    return c->f64_xfer_slot;
}

static void emit_copy(Rv32ISelCtx *c, MVReg dst, MVReg src, uint8_t w, uint32_t loc) {
    MInst cp = minst_make(MI_COPY, w, loc);
    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(src);
    emit(c, cp);
}

static void emit_li(Rv32ISelCtx *c, MVReg dst, int32_t val, uint32_t loc) {
    MInst li = minst_make(MI_RV32_LI, 4u, loc);
    li.op_count = 2u; li.ops[0] = mop_vreg_def(dst); li.ops[1] = mop_imm((int64_t)val);
    emit(c, li);
}

static void emit_imm_fp(Rv32ISelCtx *c, MVReg dst, uint64_t bits, uint8_t width, uint32_t loc) {
    if (width == 4u) {
        MVReg gp = new_gp(c);
        emit_li(c, gp, (int32_t)(uint32_t)(bits & 0xFFFFFFFFu), loc);
        MInst mv = minst_make(MI_RV32_FMV_W_X, 4u, loc);
        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(gp);
        emit(c, mv);
    } else {
        MInst lf = minst_make(MI_RV32_LOAD_IMM_F64, 8u, loc);
        lf.op_count = 2u; lf.ops[0] = mop_vreg_def(dst); lf.ops[1] = mop_imm((int64_t)bits);
        emit(c, lf);
    }
}

static void lookup_symbol_meta(const PirModule *mod, const char *name,
                               bool *out_is_func, bool *out_is_tls, bool *out_use_got) {
    *out_is_func = false;
    *out_is_tls  = false;
    *out_use_got = false;
    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (strcmp(g->name, name) == 0) {
            *out_is_tls  = (g->seg == PIR_GSEG_TLS);
            *out_use_got = (g->linkage == PIR_LINK_EXTERN || g->linkage == PIR_LINK_WEAK);
            return;
        }
    }
    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        const PirFunction *f = &mod->funcs.data[i];
        if (strcmp(f->name, name) == 0) {
            *out_is_func = true;
            *out_use_got = (f->linkage == PIR_LINK_EXTERN || f->linkage == PIR_LINK_WEAK);
            return;
        }
    }
    *out_use_got = true;
}

static void emit_sym_addr(Rv32ISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false, use_got = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls, &use_got);

    MVReg base = (offset != 0) ? new_gp(c) : dst;
    MInst a = minst_make(is_tls ? MI_RV32_TLS_ADDR : MI_RV32_SYM_ADDR, 4u, loc);
    a.op_count = 2u;
    a.ops[0]   = mop_vreg_def(base);
    a.ops[1]   = mop_sym(name, 0, is_func, is_tls, use_got);
    emit(c, a);

    if (offset != 0) {
        int32_t off32 = (int32_t)offset;
        if (fits_simm12(off32)) {
            MInst ad = minst_make(MI_RV32_ADDI, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_imm(off32);
            emit(c, ad);
        } else {
            MVReg off_r = new_gp(c);
            emit_li(c, off_r, off32, loc);
            MInst ad = minst_make(MI_RV32_ADD, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_vreg_use(off_r);
            emit(c, ad);
        }
    }
}

static MVReg mat_op(Rv32ISelCtx *c, const PirOperand *op, uint32_t loc) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            return c->val_vreg[op->u.val];
        case PIR_OPER_IMM_INT: {
            MVReg r = new_gp(c);
            emit_li(c, r, (int32_t)eval_imm_u32(op), loc);
            return r;
        }
        case PIR_OPER_IMM_FLOAT: {
            uint8_t w = rv32_vreg_bytes(op->type);
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

static MVReg emit_sext_sub(Rv32ISelCtx *c, MVReg src, uint8_t bits, uint32_t loc) {
    uint8_t sh = (uint8_t)(32u - bits);
    MVReg t = new_gp(c);
    MInst s1 = minst_make(MI_RV32_SLLI, 4u, loc);
    s1.op_count = 3u; s1.ops[0] = mop_vreg_def(t); s1.ops[1] = mop_vreg_use(src); s1.ops[2] = mop_imm(sh);
    emit(c, s1);
    MVReg r = new_gp(c);
    MInst s2 = minst_make(MI_RV32_SRAI, 4u, loc);
    s2.op_count = 3u; s2.ops[0] = mop_vreg_def(r); s2.ops[1] = mop_vreg_use(t); s2.ops[2] = mop_imm(sh);
    emit(c, s2);
    return r;
}

static MVReg emit_zext_sub(Rv32ISelCtx *c, MVReg src, uint8_t bits, uint32_t loc) {
    if (bits == 8u) {
        MVReg r = new_gp(c);
        MInst a = minst_make(MI_RV32_ANDI, 4u, loc);
        a.op_count = 3u; a.ops[0] = mop_vreg_def(r); a.ops[1] = mop_vreg_use(src); a.ops[2] = mop_imm(0xFF);
        emit(c, a);
        return r;
    }
    /* 16-bit: slli 16 + srli 16 */
    MVReg t = new_gp(c);
    MInst s1 = minst_make(MI_RV32_SLLI, 4u, loc);
    s1.op_count = 3u; s1.ops[0] = mop_vreg_def(t); s1.ops[1] = mop_vreg_use(src); s1.ops[2] = mop_imm(16);
    emit(c, s1);
    MVReg r = new_gp(c);
    MInst s2 = minst_make(MI_RV32_SRLI, 4u, loc);
    s2.op_count = 3u; s2.ops[0] = mop_vreg_def(r); s2.ops[1] = mop_vreg_use(t); s2.ops[2] = mop_imm(16);
    emit(c, s2);
    return r;
}

static MVReg normalize_int(Rv32ISelCtx *c, MVReg src, PirType ty, bool is_signed, uint32_t loc) {
    if (ty.kind != PIR_TYPE_SCALAR) return src;
    if (ty.u.scalar == PIR_SCALAR_I8) {
        return is_signed ? emit_sext_sub(c, src, 8u, loc) : emit_zext_sub(c, src, 8u, loc);
    }
    if (ty.u.scalar == PIR_SCALAR_I16) {
        return is_signed ? emit_sext_sub(c, src, 16u, loc) : emit_zext_sub(c, src, 16u, loc);
    }
    return src;
}

/* Emit conditional trap: if (rs1 <cond> rs2) trap! (rs2 == MVREG_NONE uses x0/zero). */
static void emit_bcc_trap(Rv32ISelCtx *c, MCond cond, MVReg rs1, MVReg rs2, uint32_t loc) {
    uint32_t trap_blk = mfunc_new_block(c->mf, "trap");
    uint32_t ok_blk   = mfunc_new_block(c->mf, "ok");

    MInst br = minst_make(MI_RV32_BCC, 4u, loc);
    br.op_count = 4u;
    br.ops[0] = mop_cond(cond);
    br.ops[1] = mop_vreg_use(rs1);
    br.ops[2] = (rs2 == MVREG_NONE) ? mop_preg_use(RV32_ZERO) : mop_vreg_use(rs2);
    br.ops[3] = mop_block(trap_blk);
    emit(c, br);

    MInst j_ok = minst_make(MI_RV32_J, 4u, loc);
    j_ok.op_count = 1u; j_ok.ops[0] = mop_block(ok_blk);
    emit(c, j_ok);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_blk);
    mfunc_add_edge(c->mf, c->cur_mblk, ok_blk);

    MInst tr = minst_make(MI_RV32_UNIMP, 4u, loc);
    mblock_push(&c->mf->blocks.data[trap_blk], tr);

    c->cur_mblk = ok_blk;
}

/* =========================================================================
 * Parallel Copy Emission for Block Arguments
 * ========================================================================= */

static void emit_branch_pcopy(Rv32ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    if (bt->arg_count == 0u) return;
    const PirBlock *dst_blk = &c->fn->blocks.data[bt->target];
    uint32_t n = bt->arg_count;
    MPCopyEntry *entries = (MPCopyEntry *)calloc(n, sizeof(MPCopyEntry));
    if (!entries) abort();

    uint32_t count = 0u;
    for (uint32_t k = 0u; k < n; ++k) {
        PirValueId dv = dst_blk->params.data[k];
        MVReg dst_r   = c->val_vreg[dv];
        PirType dty   = c->fn->values.data[dv].type;
        const PirOperand *src = &bt->args[k];

        MPCopyEntry *e = &entries[count++];
        e->dst       = dst_r;
        e->rclass    = rv32_rclass(dty);
        e->byte_size = rv32_vreg_bytes(dty);

        if (src->kind == PIR_OPER_VAL) {
            e->src_is_imm = false;
            e->src        = c->val_vreg[src->u.val];
        } else if (src->kind == PIR_OPER_IMM_INT) {
            e->src_is_imm = true;
            e->imm_bits   = eval_imm_u32(src);
        } else if (src->kind == PIR_OPER_IMM_FLOAT) {
            e->src_is_imm = true;
            e->imm_bits   = src->u.imm_float.bits;
        } else if (src->kind == PIR_OPER_SYM) {
            MVReg tmp = new_gp(c);
            emit_sym_addr(c, tmp, src->u.sym.name, src->u.sym.offset, loc);
            e->src_is_imm = false;
            e->src        = tmp;
        }
    }

    uint32_t pc_id = mfunc_add_pcopy(c->mf, entries, count);
    free(entries);

    MInst pc = minst_make(MI_PCOPY, 4u, loc);
    pc.aux_id = pc_id;
    emit(c, pc);
}

/* Return MBlock target for a conditional branch edge, creating a trampoline if it carries block args. */
static uint32_t resolve_edge_target(Rv32ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    uint32_t dst_b = c->pir_to_mblk[bt->target];
    if (bt->arg_count == 0u) return dst_b;
    uint32_t saved_blk = c->cur_mblk;
    uint32_t edge_blk  = mfunc_new_block(c->mf, "edge");
    c->cur_mblk = edge_blk;
    emit_branch_pcopy(c, bt, loc);
    MInst j = minst_make(MI_RV32_J, 4u, loc);
    j.op_count = 1u; j.ops[0] = mop_block(dst_b);
    emit(c, j);
    mfunc_add_edge(c->mf, edge_blk, dst_b);
    c->cur_mblk = saved_blk;
    return edge_blk;
}

/* =========================================================================
 * Calls (ILP32D Calling Convention + Variadic + Multi-Reg Return)
 * ========================================================================= */

static bool try_isel_pure_math_call(Rv32ISelCtx *c, const PirInst *inst, uint32_t loc) {
    if (!pir_inst_is_pure_math_call(c->mod, c->fn, inst)) return false;
    const char *cn = inst->operands[0].u.sym.name;
    MVReg dst = c->val_vreg[inst->results[0]];
    MVReg src = mat_op(c, &inst->operands[1], loc);
    uint8_t w = (strcmp(cn, "sqrtf") == 0 || strcmp(cn, "fabsf") == 0) ? 4u : 8u;
    MOpcode op = (strcmp(cn, "sqrt") == 0 || strcmp(cn, "sqrtf") == 0)
                 ? MI_RV32_FSQRT : MI_RV32_FABS;
    MInst mi = minst_make(op, w, loc);
    mi.op_count = 2u; mi.ops[0] = mop_vreg_def(dst); mi.ops[1] = mop_vreg_use(src);
    emit(c, mi);
    return true;
}

static MVReg emit_normalize_subword(Rv32ISelCtx *c, MVReg v, PirType ty, bool is_signed, uint32_t loc) {
    if (ty.kind != PIR_TYPE_SCALAR ||
        (ty.u.scalar != PIR_SCALAR_I8 && ty.u.scalar != PIR_SCALAR_I16)) {
        return v;
    }
    uint32_t sh = (ty.u.scalar == PIR_SCALAR_I8) ? 24u : 16u;
    if (!is_signed && ty.u.scalar == PIR_SCALAR_I8) {
        MVReg res = new_gp(c);
        MInst an = minst_make(MI_RV32_ANDI, 4u, loc);
        an.op_count = 3u; an.ops[0] = mop_vreg_def(res);
        an.ops[1] = mop_vreg_use(v); an.ops[2] = mop_imm(255);
        emit(c, an);
        return res;
    }
    MVReg tmp = new_gp(c), res = new_gp(c);
    MInst sl = minst_make(MI_RV32_SLLI, 4u, loc);
    sl.op_count = 3u; sl.ops[0] = mop_vreg_def(tmp);
    sl.ops[1] = mop_vreg_use(v); sl.ops[2] = mop_imm((int64_t)sh);
    emit(c, sl);
    MInst sr = minst_make(is_signed ? MI_RV32_SRAI : MI_RV32_SRLI, 4u, loc);
    sr.op_count = 3u; sr.ops[0] = mop_vreg_def(res);
    sr.ops[1] = mop_vreg_use(tmp); sr.ops[2] = mop_imm((int64_t)sh);
    emit(c, sr);
    return res;
}

static void emit_f64_to_gp_pair(Rv32ISelCtx *c, MVReg fval, MVReg *out_lo, MVReg *out_hi, uint32_t loc) {
    uint32_t sl = get_f64_xfer_slot(c);
    MInst st = minst_make(MI_STORE_SLOT, 8u, loc);
    st.op_count = 2u; st.ops[0] = mop_vreg_use(fval); st.ops[1] = mop_slot(sl);
    emit(c, st);
    MVReg addr = new_gp(c);
    MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
    fa.op_count = 3u; fa.ops[0] = mop_vreg_def(addr); fa.ops[1] = mop_slot(sl); fa.ops[2] = mop_imm(0);
    emit(c, fa);
    *out_lo = new_gp(c);
    *out_hi = new_gp(c);
    MInst l0 = minst_make(MI_RV32_LD, 4u, loc);
    l0.op_count = 3u; l0.ops[0] = mop_vreg_def(*out_lo); l0.ops[1] = mop_vreg_use(addr); l0.ops[2] = mop_imm(0);
    emit(c, l0);
    MInst l1 = minst_make(MI_RV32_LD, 4u, loc);
    l1.op_count = 3u; l1.ops[0] = mop_vreg_def(*out_hi); l1.ops[1] = mop_vreg_use(addr); l1.ops[2] = mop_imm(4);
    emit(c, l1);
}

static void emit_rv32_multi_ret_offsets(Rv32ISelCtx *c, const PirCabiRetInfo *cr, uint64_t out_offs[4]) {
    uint64_t cur_off = 0u;
    for (uint32_t k = 0u; k < cr->reg_count && k < 4u; ++k) {
        if (cr->has_reg_offsets) {
            out_offs[k] = cr->reg_offsets[k];
        } else {
            PirTypeLayout rly = pir_type_layout(c->mod, c->target, cr->regs[k]);
            cur_off = align_up_u64(cur_off, rly.align);
            out_offs[k] = cur_off;
            cur_off += rly.size;
        }
    }
}

static void emit_rv32_copy_bytes_mem(Rv32ISelCtx *c, MVReg dst_base, uint64_t dst_off,
                                     MVReg src_base, uint64_t src_off, uint64_t nbytes, uint32_t loc) {
    for (uint64_t bi = 0u; bi < nbytes; ++bi) {
        MVReg b = new_gp(c);
        MInst lb = minst_make(MI_RV32_LD, 1u, loc);
        lb.is_signed = false;
        lb.op_count = 3u; lb.ops[0] = mop_vreg_def(b);
        lb.ops[1] = mop_vreg_use(src_base); lb.ops[2] = mop_imm((int64_t)(src_off + bi));
        emit(c, lb);
        MInst sb = minst_make(MI_RV32_ST, 1u, loc);
        sb.op_count = 3u; sb.ops[0] = mop_vreg_use(b);
        sb.ops[1] = mop_vreg_use(dst_base); sb.ops[2] = mop_imm((int64_t)(dst_off + bi));
        emit(c, sb);
    }
}

static void isel_call(Rv32ISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);
    if (!is_ind && try_isel_pure_math_call(c, inst, loc)) {
        return;
    }

    c->mf->has_calls = true;

    PirType ret_ty = inst->ty;
    bool is_variadic = false;
    uint32_t fixed_params = inst->operand_count - 1u;
    const PirParamDecl *callee_params = NULL;
    uint32_t callee_param_count = 0u;
    PirCabiRetInfo cabi_ret;
    memset(&cabi_ret, 0, sizeof(cabi_ret));

    if (is_ind) {
        const PirSigDef *sig = &c->mod->sigs.data[inst->sig_id];
        is_variadic        = sig->is_variadic;
        fixed_params       = sig->param_count;
        callee_params      = sig->params;
        callee_param_count = sig->param_count;
        cabi_ret           = sig->cabi_ret;
    } else {
        const PirFunction *cal = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        is_variadic        = cal->is_variadic;
        fixed_params       = cal->param_count;
        callee_params      = cal->params;
        callee_param_count = cal->param_count;
        cabi_ret           = cal->cabi_ret;
    }

    uint32_t nargs = inst->operand_count - 1u;
    bool has_multi_out = (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) && nargs > 0u;
    MVReg hidden_out_v = MVREG_NONE;
    uint32_t arg_start = 0u;
    if (has_multi_out) {
        hidden_out_v = mat_op(c, &inst->operands[1], loc);
        arg_start = 1u;
    }

    MVReg ind_target_v = MVREG_NONE;
    if (is_ind) {
        ind_target_v = mat_op(c, &inst->operands[0], loc);
    }

    /* Evaluate all argument vregs first before writing fixed physical regs. */
    typedef struct {
        MVReg    vreg;
        MVReg    vreg_hi;     /* Second word if f64 split into GP regs */
        bool     is_f64_gp;   /* f64 passed via GP regs (variadic or FP reg overflow) */
        bool     is_f32_gp;   /* f32 passed via GP reg (FP reg overflow) */
        bool     in_reg;
        bool     hi_on_stack; /* f64 split across a7 and sp+0 */
        MPReg    preg;
        MPReg    preg_hi;
        uint64_t stk_off;
        uint8_t  width;
        bool     is_fp;
    } CallArgPlan;

    CallArgPlan *plans = nargs ? (CallArgPlan *)calloc(nargs, sizeof(CallArgPlan)) : NULL;
    if (nargs && !plans) abort();

    uint32_t gp_idx = 0u;
    uint32_t fp_idx = 0u;
    uint64_t stk_off = 0u;

    for (uint32_t i = arg_start; i < nargs; ++i) {
        const PirOperand *aop = &inst->operands[1u + i];
        PirType aty = aop->type;
        bool is_va_arg = is_variadic && (i >= fixed_params);
        CallArgPlan *pl = &plans[i];

        /* Variadic f32 promotes to f64 unless explicitly suppressed. */
        bool promote_f32 = is_va_arg &&
                           aty.kind == PIR_TYPE_SCALAR &&
                           aty.u.scalar == PIR_SCALAR_F32 &&
                           !aop->call_no_f32_promote;

        MVReg v = mat_op(c, aop, loc);
        if (promote_f32) {
            MVReg d = new_fp(c, 8u);
            MInst cv = minst_make(MI_RV32_FCVT_D_S, 8u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(d); cv.ops[1] = mop_vreg_use(v);
            emit(c, cv);
            v = d;
            aty = pir_type_scalar(PIR_SCALAR_F64);
        }

        if (aty.kind == PIR_TYPE_SCALAR && aty.u.scalar == PIR_SCALAR_F32) {
            pl->width = 4u;
            pl->is_fp = true;
            pl->vreg  = v;
            if (!is_va_arg && fp_idx < 8u) {
                pl->in_reg = true;
                pl->preg   = RV32_FP(10u + fp_idx++);
            } else if (gp_idx < 8u) {
                MVReg gv = new_gp(c);
                MInst mv = minst_make(MI_RV32_FMV_X_W, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(gv); mv.ops[1] = mop_vreg_use(v);
                emit(c, mv);
                pl->vreg      = gv;
                pl->is_fp     = false;
                pl->is_f32_gp = true;
                pl->in_reg    = true;
                pl->preg      = RV32_GP(10u + gp_idx++);
            } else {
                pl->in_reg  = false;
                pl->stk_off = stk_off;
                stk_off += 4u;
            }
        } else if (aty.kind == PIR_TYPE_SCALAR && aty.u.scalar == PIR_SCALAR_F64) {
            pl->width = 8u;
            pl->is_fp = true;
            pl->vreg  = v;
            if (!is_va_arg && fp_idx < 8u) {
                pl->in_reg = true;
                pl->preg   = RV32_FP(10u + fp_idx++);
            } else {
                /* Variadic f64 (always GP, even-reg aligned) or non-variadic f64 overflow. */
                if (is_va_arg && (gp_idx & 1u) != 0u && gp_idx < 8u) {
                    gp_idx++;
                }
                if (gp_idx + 2u <= 8u) {
                    MVReg vlo = MVREG_NONE, vhi = MVREG_NONE;
                    emit_f64_to_gp_pair(c, v, &vlo, &vhi, loc);
                    pl->vreg      = vlo;
                    pl->vreg_hi   = vhi;
                    pl->is_fp     = false;
                    pl->is_f64_gp = true;
                    pl->in_reg    = true;
                    pl->preg      = RV32_GP(10u + gp_idx);
                    pl->preg_hi   = RV32_GP(10u + gp_idx + 1u);
                    gp_idx += 2u;
                } else if (!is_va_arg && gp_idx == 7u) {
                    /* Split across a7 (lo) and sp+0 (hi). */
                    MVReg vlo = MVREG_NONE, vhi = MVREG_NONE;
                    emit_f64_to_gp_pair(c, v, &vlo, &vhi, loc);
                    pl->vreg        = vlo;
                    pl->vreg_hi     = vhi;
                    pl->is_fp       = false;
                    pl->is_f64_gp   = true;
                    pl->in_reg      = true;
                    pl->hi_on_stack = true;
                    pl->preg        = RV32_A7;
                    pl->stk_off     = stk_off;
                    gp_idx = 8u;
                    stk_off += 4u;
                } else {
                    stk_off = align_up_u64(stk_off, 8u);
                    pl->in_reg  = false;
                    pl->stk_off = stk_off;
                    stk_off += 8u;
                }
            }
        } else {
            /* GP scalar (i8, i16, i32, ptr): honor signext/zeroext param attributes. */
            if (aty.kind == PIR_TYPE_SCALAR &&
                (aty.u.scalar == PIR_SCALAR_I8 || aty.u.scalar == PIR_SCALAR_I16)) {
                uint32_t pattr = (!is_va_arg && callee_params && i < callee_param_count)
                                 ? callee_params[i].attrs : 0u;
                if ((pattr & PIR_PATTR_SIGNEXT) != 0u) {
                    v = emit_normalize_subword(c, v, aty, true, loc);
                } else if ((pattr & PIR_PATTR_ZEROEXT) != 0u || is_va_arg) {
                    v = emit_normalize_subword(c, v, aty, false, loc);
                }
            }
            pl->width = 4u;
            pl->is_fp = false;
            pl->vreg  = v;
            if (aop->call_stack_align >= 8u) {
                if (is_va_arg && gp_idx < 8u && (gp_idx & 1u) != 0u) {
                    gp_idx++;
                }
            }
            if (gp_idx < 8u && !aop->call_force_stack) {
                pl->in_reg = true;
                pl->preg   = RV32_GP(10u + gp_idx++);
            } else {
                uint64_t al = (aop->call_stack_align > 4u) ? (uint64_t)aop->call_stack_align : 4u;
                stk_off = align_up_u64(stk_off, al);
                pl->in_reg  = false;
                pl->stk_off = stk_off;
                stk_off += 4u;
            }
        }
    }

    uint64_t call_out_bytes = align_up_u64(stk_off, 16u);
    if (call_out_bytes > c->mf->max_out_args_bytes) {
        c->mf->max_out_args_bytes = call_out_bytes;
    }

    if (call_out_bytes > 0u) {
        MInst adj_d = minst_make(MI_CALL_ADJ_DOWN, 4u, loc);
        adj_d.op_count = 1u; adj_d.ops[0] = mop_imm((int64_t)call_out_bytes);
        emit(c, adj_d);
    }

    /* Emit stack outgoing stores first. */
    for (uint32_t i = arg_start; i < nargs; ++i) {
        const CallArgPlan *pl = &plans[i];
        if (!pl->in_reg) {
            MInst st = minst_make(MI_STORE_OUTARG, pl->width, loc);
            st.op_count = 2u; st.ops[0] = mop_vreg_use(pl->vreg); st.ops[1] = mop_imm((int64_t)pl->stk_off);
            emit(c, st);
        } else if (pl->hi_on_stack) {
            MInst st = minst_make(MI_STORE_OUTARG, 4u, loc);
            st.op_count = 2u; st.ops[0] = mop_vreg_use(pl->vreg_hi); st.ops[1] = mop_imm((int64_t)pl->stk_off);
            emit(c, st);
        }
    }

    /* Move indirect callee pointer to t3 (caller-saved, never a0..a7!) before populating a0..a7. */
    if (is_ind) {
        MInst mv = minst_make(MI_COPY, 4u, loc);
        mv.op_count = 2u; mv.ops[0] = mop_preg_def(RV32_T3); mv.ops[1] = mop_vreg_use(ind_target_v);
        emit(c, mv);
    }

    /* Copy register arguments into a0..a7 and fa0..fa7. */
    for (uint32_t i = arg_start; i < nargs; ++i) {
        const CallArgPlan *pl = &plans[i];
        if (!pl->in_reg) continue;
        MInst cp = minst_make(MI_COPY, pl->is_f64_gp ? 4u : pl->width, loc);
        cp.op_count = 2u; cp.ops[0] = mop_preg_def(pl->preg); cp.ops[1] = mop_vreg_use(pl->vreg);
        emit(c, cp);
        if (pl->is_f64_gp && !pl->hi_on_stack) {
            MInst cp2 = minst_make(MI_COPY, 4u, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_preg_def(pl->preg_hi); cp2.ops[1] = mop_vreg_use(pl->vreg_hi);
            emit(c, cp2);
        }
    }
    free(plans);

    /* Emit the call instruction. */
    if (is_ind) {
        MInst call = minst_make(MI_RV32_CALL_IND, 4u, loc);
        call.is_call  = true;
        call.op_count = 1u;
        call.ops[0]   = mop_preg_use(RV32_T3);
        emit(c, call);
    } else {
        const PirFunction *cal = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        MInst call = minst_make(MI_RV32_CALL, 4u, loc);
        call.is_call  = true;
        call.op_count = 1u;
        call.ops[0]   = mop_sym(cal->name, 0, true, false, false);
        emit(c, call);
    }

    if (call_out_bytes > 0u) {
        MInst adj_u = minst_make(MI_CALL_ADJ_UP, 4u, loc);
        adj_u.op_count = 1u; adj_u.ops[0] = mop_imm((int64_t)call_out_bytes);
        emit(c, adj_u);
    }

    /* Handle multi-register C ABI return (e.g., i64 in (a0, a1), {f32,f32} in (fa0, fa1), {i32,f32} in (a0, fa0)). */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && hidden_out_v != MVREG_NONE) {
        uint32_t rg = 0u, rf = 0u;
        MVReg saved_v[4];
        uint8_t saved_w[4];
        bool saved_fp[4];
        uint64_t saved_off[4];
        emit_rv32_multi_ret_offsets(c, &cabi_ret, saved_off);
        for (uint32_t k = 0u; k < cabi_ret.reg_count; ++k) {
            PirType rt = cabi_ret.regs[k];
            saved_fp[k] = (rv32_rclass(rt) == MREG_CLASS_FP);
            saved_w[k]  = rv32_mem_bytes(rt);
            MPReg pr = saved_fp[k] ? RV32_FP(10u + rf++) : RV32_GP(10u + rg++);
            saved_v[k] = new_vreg_for_type(c, rt);
            MInst cp = minst_make(MI_COPY, rv32_vreg_bytes(rt), loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(saved_v[k]); cp.ops[1] = mop_preg_use(pr);
            emit(c, cp);
        }
        uint32_t agg_al = cabi_ret.agg_align > 0u ? cabi_ret.agg_align : 1u;
        for (uint32_t k = 0u; k < cabi_ret.reg_count; ++k) {
            uint64_t off = saved_off[k];
            uint64_t rem = (cabi_ret.agg_size > off) ? (cabi_ret.agg_size - off) : 0u;
            bool unaligned = (saved_w[k] > 1u) && ((off % saved_w[k]) != 0u || agg_al < saved_w[k]);
            if (!saved_fp[k] && (rem < saved_w[k] || unaligned)) {
                uint64_t nbytes = (rem < saved_w[k]) ? rem : saved_w[k];
                for (uint64_t bi = 0u; bi < nbytes; ++bi) {
                    MVReg bv = saved_v[k];
                    if (bi > 0u) {
                        MVReg sh = new_gp(c);
                        MInst sr = minst_make(MI_RV32_SRLI, 4u, loc);
                        sr.op_count = 3u; sr.ops[0] = mop_vreg_def(sh); sr.ops[1] = mop_vreg_use(saved_v[k]);
                        sr.ops[2] = mop_imm((int64_t)(bi * 8u));
                        emit(c, sr);
                        bv = sh;
                    }
                    MInst sb = minst_make(MI_RV32_ST, 1u, loc);
                    sb.op_count = 3u; sb.ops[0] = mop_vreg_use(bv);
                    sb.ops[1] = mop_vreg_use(hidden_out_v); sb.ops[2] = mop_imm((int64_t)(off + bi));
                    emit(c, sb);
                }
            } else if (saved_fp[k] && unaligned) {
                uint32_t sl = get_f64_xfer_slot(c);
                MInst st = minst_make(MI_STORE_SLOT, saved_w[k], loc);
                st.op_count = 2u; st.ops[0] = mop_vreg_use(saved_v[k]); st.ops[1] = mop_slot(sl);
                emit(c, st);
                MVReg saddr = new_gp(c);
                MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
                fa.op_count = 3u; fa.ops[0] = mop_vreg_def(saddr); fa.ops[1] = mop_slot(sl); fa.ops[2] = mop_imm(0);
                emit(c, fa);
                emit_rv32_copy_bytes_mem(c, hidden_out_v, off, saddr, 0u, saved_w[k], loc);
            } else {
                MInst st = minst_make(saved_fp[k] ? MI_RV32_FST : MI_RV32_ST, saved_w[k], loc);
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
        MPReg rpreg = (rv32_rclass(ret_ty) == MREG_CLASS_FP) ? RV32_FA0 : RV32_A0;
        MInst cp = minst_make(MI_COPY, rv32_vreg_bytes(ret_ty), loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(rpreg);
        emit(c, cp);
    }
}

/* =========================================================================
 * Single Instruction Selection
 * ========================================================================= */

static void isel_inst(Rv32ISelCtx *c, const PirInst *inst) {
    uint32_t loc = inst->loc_idx;

    switch (inst->op) {
        case PIR_OP_CONST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            if (inst->operands[0].kind == PIR_OPER_IMM_FLOAT) {
                emit_imm_fp(c, dst, inst->operands[0].u.imm_float.bits, rv32_vreg_bytes(inst->ty), loc);
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
                    if (fits_simm12(adj)) {
                        MInst ai = minst_make(MI_RV32_ADDI, 4u, loc);
                        ai.op_count = 3u; ai.ops[0] = mop_vreg_def(dst);
                        ai.ops[1] = mop_vreg_use(lhs); ai.ops[2] = mop_imm(adj);
                        emit(c, ai);
                        return;
                    }
                }
                MVReg rhs = mat_op(c, &inst->operands[1], loc);
                MInst mi = minst_make(is_add ? MI_RV32_ADD : MI_RV32_SUB, 4u, loc);
                mi.op_count = 3u; mi.ops[0] = mop_vreg_def(dst);
                mi.ops[1] = mop_vreg_use(lhs); mi.ops[2] = mop_vreg_use(rhs);
                emit(c, mi);
                return;
            }

            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, trap_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, trap_s, loc);
            MInst mi = minst_make(is_add ? MI_RV32_ADD : MI_RV32_SUB, 4u, loc);
            mi.op_count = 3u; mi.ops[0] = mop_vreg_def(dst);
            mi.ops[1] = mop_vreg_use(lhs); mi.ops[2] = mop_vreg_use(rhs);
            emit(c, mi);

            if (sc == PIR_SCALAR_I8 || sc == PIR_SCALAR_I16) {
                uint8_t bits = (sc == PIR_SCALAR_I8) ? 8u : 16u;
                MVReg chk = trap_s ? emit_sext_sub(c, dst, bits, loc) : emit_zext_sub(c, dst, bits, loc);
                emit_bcc_trap(c, MCOND_NE, dst, chk, loc);
            } else if (trap_u) {
                if (is_add) {
                    emit_bcc_trap(c, MCOND_CC, dst, lhs, loc); /* bltu dst, lhs */
                } else {
                    emit_bcc_trap(c, MCOND_CC, lhs, rhs, loc); /* bltu lhs, rhs */
                }
            } else {
                /* Signed 32-bit overflow:
                 * add: ((dst ^ lhs) & (dst ^ rhs)) < 0
                 * sub: ((lhs ^ rhs) & (lhs ^ dst)) < 0 */
                MVReg x1 = new_gp(c), x2 = new_gp(c), x3 = new_gp(c);
                MInst e1 = minst_make(MI_RV32_XOR, 4u, loc);
                e1.op_count = 3u; e1.ops[0] = mop_vreg_def(x1);
                e1.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                e1.ops[2] = mop_vreg_use(is_add ? lhs : rhs);
                emit(c, e1);
                MInst e2 = minst_make(MI_RV32_XOR, 4u, loc);
                e2.op_count = 3u; e2.ops[0] = mop_vreg_def(x2);
                e2.ops[1] = mop_vreg_use(is_add ? dst : lhs);
                e2.ops[2] = mop_vreg_use(is_add ? rhs : dst);
                emit(c, e2);
                MInst a3 = minst_make(MI_RV32_AND, 4u, loc);
                a3.op_count = 3u; a3.ops[0] = mop_vreg_def(x3);
                a3.ops[1] = mop_vreg_use(x1); a3.ops[2] = mop_vreg_use(x2);
                emit(c, a3);
                emit_bcc_trap(c, MCOND_LT, x3, MVREG_NONE, loc);
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

            MInst mul = minst_make(MI_RV32_MUL, 4u, loc);
            mul.op_count = 3u; mul.ops[0] = mop_vreg_def(dst);
            mul.ops[1] = mop_vreg_use(lhs); mul.ops[2] = mop_vreg_use(rhs);
            emit(c, mul);

            if (!trap_s && !trap_u) return;

            if (sc == PIR_SCALAR_I8 || sc == PIR_SCALAR_I16) {
                uint8_t bits = (sc == PIR_SCALAR_I8) ? 8u : 16u;
                MVReg chk = trap_s ? emit_sext_sub(c, dst, bits, loc) : emit_zext_sub(c, dst, bits, loc);
                emit_bcc_trap(c, MCOND_NE, dst, chk, loc);
            } else if (trap_u) {
                MVReg hi = new_gp(c);
                MInst mh = minst_make(MI_RV32_MULHU, 4u, loc);
                mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi);
                mh.ops[1] = mop_vreg_use(lhs); mh.ops[2] = mop_vreg_use(rhs);
                emit(c, mh);
                emit_bcc_trap(c, MCOND_NE, hi, MVREG_NONE, loc);
            } else {
                MVReg hi = new_gp(c), sgn = new_gp(c);
                MInst mh = minst_make(MI_RV32_MULH, 4u, loc);
                mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi);
                mh.ops[1] = mop_vreg_use(lhs); mh.ops[2] = mop_vreg_use(rhs);
                emit(c, mh);
                MInst sr = minst_make(MI_RV32_SRAI, 4u, loc);
                sr.op_count = 3u; sr.ops[0] = mop_vreg_def(sgn);
                sr.ops[1] = mop_vreg_use(dst); sr.ops[2] = mop_imm(31);
                emit(c, sr);
                emit_bcc_trap(c, MCOND_NE, hi, sgn, loc);
            }
            return;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            MVReg rlo = c->val_vreg[inst->results[0]];
            MVReg rhi = c->val_vreg[inst->results[1]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MInst mlo = minst_make(MI_RV32_MUL, 4u, loc);
            mlo.op_count = 3u; mlo.ops[0] = mop_vreg_def(rlo);
            mlo.ops[1] = mop_vreg_use(lhs); mlo.ops[2] = mop_vreg_use(rhs);
            emit(c, mlo);
            MInst mhi = minst_make(inst->op == PIR_OP_MUL_WIDE_S ? MI_RV32_MULH : MI_RV32_MULHU, 4u, loc);
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
            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, is_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, is_s, loc);

            /* RV32M hardware div/rem never traps; emit explicit PIR [trap]/[wrap] guards. */
            if (!(inst->flags & PIR_FLAG_UNSAFE)) {
                emit_bcc_trap(c, MCOND_EQ, rhs, MVREG_NONE, loc);
                if (is_s && !(inst->flags & PIR_FLAG_WRAP)) {
                    int32_t min_v = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? INT8_MIN
                                  : (inst->ty.u.scalar == PIR_SCALAR_I16) ? INT16_MIN
                                                                          : INT32_MIN;
                    MVReg cmin = new_gp(c), cm1 = new_gp(c);
                    MVReg d1 = new_gp(c), d2 = new_gp(c), both = new_gp(c);
                    emit_li(c, cmin, min_v, loc);
                    emit_li(c, cm1, -1, loc);
                    MInst x1 = minst_make(MI_RV32_XOR, 4u, loc);
                    x1.op_count = 3u; x1.ops[0] = mop_vreg_def(d1); x1.ops[1] = mop_vreg_use(lhs); x1.ops[2] = mop_vreg_use(cmin);
                    emit(c, x1);
                    MInst x2 = minst_make(MI_RV32_XOR, 4u, loc);
                    x2.op_count = 3u; x2.ops[0] = mop_vreg_def(d2); x2.ops[1] = mop_vreg_use(rhs); x2.ops[2] = mop_vreg_use(cm1);
                    emit(c, x2);
                    MInst o1 = minst_make(MI_RV32_OR, 4u, loc);
                    o1.op_count = 3u; o1.ops[0] = mop_vreg_def(both); o1.ops[1] = mop_vreg_use(d1); o1.ops[2] = mop_vreg_use(d2);
                    emit(c, o1);
                    emit_bcc_trap(c, MCOND_EQ, both, MVREG_NONE, loc);
                }
            }
            MOpcode dop = is_s ? (is_rem ? MI_RV32_REM : MI_RV32_DIV)
                               : (is_rem ? MI_RV32_REMU : MI_RV32_DIVU);
            MInst dv = minst_make(dop, 4u, loc);
            dv.op_count = 3u; dv.ops[0] = mop_vreg_def(dst);
            dv.ops[1] = mop_vreg_use(lhs); dv.ops[2] = mop_vreg_use(rhs);
            emit(c, dv);
            return;
        }

        case PIR_OP_AND:
        case PIR_OP_OR:
        case PIR_OP_XOR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                int32_t imm = (int32_t)eval_imm_u32(&inst->operands[1]);
                if (fits_simm12(imm)) {
                    MOpcode iop = (inst->op == PIR_OP_AND) ? MI_RV32_ANDI
                                : (inst->op == PIR_OP_OR)  ? MI_RV32_ORI : MI_RV32_XORI;
                    MInst bi = minst_make(iop, 4u, loc);
                    bi.op_count = 3u; bi.ops[0] = mop_vreg_def(dst);
                    bi.ops[1] = mop_vreg_use(lhs); bi.ops[2] = mop_imm(imm);
                    emit(c, bi);
                    return;
                }
            }
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            MOpcode rop = (inst->op == PIR_OP_AND) ? MI_RV32_AND
                        : (inst->op == PIR_OP_OR)  ? MI_RV32_OR : MI_RV32_XOR;
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
                    MInst tr = minst_make(MI_RV32_UNIMP, 4u, loc);
                    emit(c, tr);
                    emit_li(c, dst, 0, loc);
                    return;
                }
                sh &= (bits - 1u);
                MOpcode sop = (inst->op == PIR_OP_SHL) ? MI_RV32_SLLI
                            : (inst->op == PIR_OP_SHR) ? MI_RV32_SRLI : MI_RV32_SRAI;
                MInst si = minst_make(sop, 4u, loc);
                si.op_count = 3u; si.ops[0] = mop_vreg_def(dst);
                si.ops[1] = mop_vreg_use(lhs); si.ops[2] = mop_imm((int64_t)sh);
                emit(c, si);
                return;
            }
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
            if (inst->flags & PIR_FLAG_TRAP) {
                MVReg lim = new_gp(c);
                emit_li(c, lim, (int32_t)bits, loc);
                emit_bcc_trap(c, MCOND_CS, rhs, lim, loc); /* bgeu rhs, bits */
            }
            if (bits < 32u) {
                MVReg m = new_gp(c);
                MInst a = minst_make(MI_RV32_ANDI, 4u, loc);
                a.op_count = 3u; a.ops[0] = mop_vreg_def(m);
                a.ops[1] = mop_vreg_use(rhs); a.ops[2] = mop_imm((int64_t)(bits - 1u));
                emit(c, a);
                rhs = m;
            }
            MOpcode sop = (inst->op == PIR_OP_SHL) ? MI_RV32_SLL
                        : (inst->op == PIR_OP_SHR) ? MI_RV32_SRL : MI_RV32_SRA;
            MInst sr = minst_make(sop, 4u, loc);
            sr.op_count = 3u; sr.ops[0] = mop_vreg_def(dst);
            sr.ops[1] = mop_vreg_use(lhs); sr.ops[2] = mop_vreg_use(rhs);
            emit(c, sr);
            return;
        }

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg lhs = mat_op(c, &inst->operands[0], loc);
            MVReg rhs = mat_op(c, &inst->operands[1], loc);
            uint8_t w = rv32_vreg_bytes(inst->ty);
            MOpcode op = (inst->op == PIR_OP_FADD) ? MI_RV32_FADD
                       : (inst->op == PIR_OP_FSUB) ? MI_RV32_FSUB
                       : (inst->op == PIR_OP_FMUL) ? MI_RV32_FMUL : MI_RV32_FDIV;
            MInst fi = minst_make(op, w, loc);
            fi.op_count = 3u; fi.ops[0] = mop_vreg_def(dst);
            fi.ops[1] = mop_vreg_use(lhs); fi.ops[2] = mop_vreg_use(rhs);
            emit(c, fi);
            return;
        }

        case PIR_OP_FNEG: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst fi = minst_make(MI_RV32_FNEG, rv32_vreg_bytes(inst->ty), loc);
            fi.op_count = 2u; fi.ops[0] = mop_vreg_def(dst); fi.ops[1] = mop_vreg_use(src);
            emit(c, fi);
            return;
        }

        case PIR_OP_CMP_EQ:   case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U:
        case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U:
        case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U: {
            MVReg dst = c->val_vreg[inst->results[0]];
            bool is_s = (inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_LE_S ||
                         inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_GE_S);
            MVReg lhs = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, is_s, loc);
            MVReg rhs = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, is_s, loc);

            if (inst->op == PIR_OP_CMP_EQ || inst->op == PIR_OP_CMP_NE) {
                MVReg xr = new_gp(c);
                MInst x = minst_make(MI_RV32_XOR, 4u, loc);
                x.op_count = 3u; x.ops[0] = mop_vreg_def(xr);
                x.ops[1] = mop_vreg_use(lhs); x.ops[2] = mop_vreg_use(rhs);
                emit(c, x);
                MInst s = minst_make(inst->op == PIR_OP_CMP_EQ ? MI_RV32_SEQZ : MI_RV32_SNEZ, 4u, loc);
                s.op_count = 2u; s.ops[0] = mop_vreg_def(dst); s.ops[1] = mop_vreg_use(xr);
                emit(c, s);
            } else if (inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_LT_U) {
                MInst s = minst_make(is_s ? MI_RV32_SLT : MI_RV32_SLTU, 4u, loc);
                s.op_count = 3u; s.ops[0] = mop_vreg_def(dst);
                s.ops[1] = mop_vreg_use(lhs); s.ops[2] = mop_vreg_use(rhs);
                emit(c, s);
            } else if (inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_GT_U) {
                MInst s = minst_make(is_s ? MI_RV32_SLT : MI_RV32_SLTU, 4u, loc);
                s.op_count = 3u; s.ops[0] = mop_vreg_def(dst);
                s.ops[1] = mop_vreg_use(rhs); s.ops[2] = mop_vreg_use(lhs);
                emit(c, s);
            } else if (inst->op == PIR_OP_CMP_LE_S || inst->op == PIR_OP_CMP_LE_U) {
                MVReg t = new_gp(c);
                MInst s = minst_make(is_s ? MI_RV32_SLT : MI_RV32_SLTU, 4u, loc);
                s.op_count = 3u; s.ops[0] = mop_vreg_def(t);
                s.ops[1] = mop_vreg_use(rhs); s.ops[2] = mop_vreg_use(lhs);
                emit(c, s);
                MInst iv = minst_make(MI_RV32_XORI, 4u, loc);
                iv.op_count = 3u; iv.ops[0] = mop_vreg_def(dst);
                iv.ops[1] = mop_vreg_use(t); iv.ops[2] = mop_imm(1);
                emit(c, iv);
            } else { /* GE_S / GE_U */
                MVReg t = new_gp(c);
                MInst s = minst_make(is_s ? MI_RV32_SLT : MI_RV32_SLTU, 4u, loc);
                s.op_count = 3u; s.ops[0] = mop_vreg_def(t);
                s.ops[1] = mop_vreg_use(lhs); s.ops[2] = mop_vreg_use(rhs);
                emit(c, s);
                MInst iv = minst_make(MI_RV32_XORI, 4u, loc);
                iv.op_count = 3u; iv.ops[0] = mop_vreg_def(dst);
                iv.ops[1] = mop_vreg_use(t); iv.ops[2] = mop_imm(1);
                emit(c, iv);
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
            MVReg a   = mat_op(c, &inst->operands[0], loc);
            MVReg b   = mat_op(c, &inst->operands[1], loc);
            uint8_t w = rv32_vreg_bytes(inst->ty);

            if (inst->op == PIR_OP_FCMP_OEQ || inst->op == PIR_OP_FCMP_OLT || inst->op == PIR_OP_FCMP_OLE) {
                MOpcode fop = (inst->op == PIR_OP_FCMP_OEQ) ? MI_RV32_FEQ
                            : (inst->op == PIR_OP_FCMP_OLT) ? MI_RV32_FLT : MI_RV32_FLE;
                MInst fc = minst_make(fop, w, loc);
                fc.op_count = 3u; fc.ops[0] = mop_vreg_def(dst); fc.ops[1] = mop_vreg_use(a); fc.ops[2] = mop_vreg_use(b);
                emit(c, fc);
            } else if (inst->op == PIR_OP_FCMP_OGT || inst->op == PIR_OP_FCMP_OGE) {
                MInst fc = minst_make(inst->op == PIR_OP_FCMP_OGT ? MI_RV32_FLT : MI_RV32_FLE, w, loc);
                fc.op_count = 3u; fc.ops[0] = mop_vreg_def(dst); fc.ops[1] = mop_vreg_use(b); fc.ops[2] = mop_vreg_use(a);
                emit(c, fc);
            } else if (inst->op == PIR_OP_FCMP_ONE) {
                MVReg t1 = new_gp(c), t2 = new_gp(c);
                MInst f1 = minst_make(MI_RV32_FLT, w, loc);
                f1.op_count = 3u; f1.ops[0] = mop_vreg_def(t1); f1.ops[1] = mop_vreg_use(a); f1.ops[2] = mop_vreg_use(b);
                emit(c, f1);
                MInst f2 = minst_make(MI_RV32_FLT, w, loc);
                f2.op_count = 3u; f2.ops[0] = mop_vreg_def(t2); f2.ops[1] = mop_vreg_use(b); f2.ops[2] = mop_vreg_use(a);
                emit(c, f2);
                MInst o = minst_make(MI_RV32_OR, 4u, loc);
                o.op_count = 3u; o.ops[0] = mop_vreg_def(dst); o.ops[1] = mop_vreg_use(t1); o.ops[2] = mop_vreg_use(t2);
                emit(c, o);
            } else if (inst->op == PIR_OP_FCMP_UNE || inst->op == PIR_OP_FCMP_UGT || inst->op == PIR_OP_FCMP_UGE) {
                MVReg t = new_gp(c);
                MOpcode fop = (inst->op == PIR_OP_FCMP_UNE) ? MI_RV32_FEQ
                            : (inst->op == PIR_OP_FCMP_UGT) ? MI_RV32_FLE : MI_RV32_FLT;
                MInst fc = minst_make(fop, w, loc);
                fc.op_count = 3u; fc.ops[0] = mop_vreg_def(t); fc.ops[1] = mop_vreg_use(a); fc.ops[2] = mop_vreg_use(b);
                emit(c, fc);
                MInst iv = minst_make(MI_RV32_XORI, 4u, loc);
                iv.op_count = 3u; iv.ops[0] = mop_vreg_def(dst); iv.ops[1] = mop_vreg_use(t); iv.ops[2] = mop_imm(1);
                emit(c, iv);
            } else if (inst->op == PIR_OP_FCMP_ULT || inst->op == PIR_OP_FCMP_ULE) {
                MVReg t = new_gp(c);
                MInst fc = minst_make(inst->op == PIR_OP_FCMP_ULT ? MI_RV32_FLE : MI_RV32_FLT, w, loc);
                fc.op_count = 3u; fc.ops[0] = mop_vreg_def(t); fc.ops[1] = mop_vreg_use(b); fc.ops[2] = mop_vreg_use(a);
                emit(c, fc);
                MInst iv = minst_make(MI_RV32_XORI, 4u, loc);
                iv.op_count = 3u; iv.ops[0] = mop_vreg_def(dst); iv.ops[1] = mop_vreg_use(t); iv.ops[2] = mop_imm(1);
                emit(c, iv);
            } else { /* PIR_OP_FCMP_UEQ */
                MVReg eq = new_gp(c), oa = new_gp(c), ob = new_gp(c), ord = new_gp(c), unord = new_gp(c);
                MInst fe = minst_make(MI_RV32_FEQ, w, loc);
                fe.op_count = 3u; fe.ops[0] = mop_vreg_def(eq); fe.ops[1] = mop_vreg_use(a); fe.ops[2] = mop_vreg_use(b);
                emit(c, fe);
                MInst fa = minst_make(MI_RV32_FEQ, w, loc);
                fa.op_count = 3u; fa.ops[0] = mop_vreg_def(oa); fa.ops[1] = mop_vreg_use(a); fa.ops[2] = mop_vreg_use(a);
                emit(c, fa);
                MInst fb = minst_make(MI_RV32_FEQ, w, loc);
                fb.op_count = 3u; fb.ops[0] = mop_vreg_def(ob); fb.ops[1] = mop_vreg_use(b); fb.ops[2] = mop_vreg_use(b);
                emit(c, fb);
                MInst an = minst_make(MI_RV32_AND, 4u, loc);
                an.op_count = 3u; an.ops[0] = mop_vreg_def(ord); an.ops[1] = mop_vreg_use(oa); an.ops[2] = mop_vreg_use(ob);
                emit(c, an);
                MInst iv = minst_make(MI_RV32_XORI, 4u, loc);
                iv.op_count = 3u; iv.ops[0] = mop_vreg_def(unord); iv.ops[1] = mop_vreg_use(ord); iv.ops[2] = mop_imm(1);
                emit(c, iv);
                MInst orr = minst_make(MI_RV32_OR, 4u, loc);
                orr.op_count = 3u; orr.ops[0] = mop_vreg_def(dst); orr.ops[1] = mop_vreg_use(eq); orr.ops[2] = mop_vreg_use(unord);
                emit(c, orr);
            }
            return;
        }

        case PIR_OP_SELECT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg cond = normalize_int(c, mat_op(c, &inst->operands[0], loc),
                                       pir_type_scalar(PIR_SCALAR_I8), false, loc);
            MVReg tv = mat_op(c, &inst->operands[1], loc);
            MVReg fv = mat_op(c, &inst->operands[2], loc);
            uint8_t w = rv32_vreg_bytes(inst->ty);

            uint32_t f_blk = mfunc_new_block(c->mf, "sel_f");
            uint32_t d_blk = mfunc_new_block(c->mf, "sel_d");

            emit_copy(c, dst, tv, w, loc);
            MInst br = minst_make(MI_RV32_BCC, 4u, loc);
            br.op_count = 4u; br.ops[0] = mop_cond(MCOND_EQ);
            br.ops[1] = mop_vreg_use(cond); br.ops[2] = mop_preg_use(RV32_ZERO);
            br.ops[3] = mop_block(f_blk);
            emit(c, br);

            MInst jd = minst_make(MI_RV32_J, 4u, loc);
            jd.op_count = 1u; jd.ops[0] = mop_block(d_blk);
            emit(c, jd);

            mfunc_add_edge(c->mf, c->cur_mblk, f_blk);
            mfunc_add_edge(c->mf, c->cur_mblk, d_blk);

            c->cur_mblk = f_blk;
            emit_copy(c, dst, fv, w, loc);
            MInst jd2 = minst_make(MI_RV32_J, 4u, loc);
            jd2.op_count = 1u; jd2.ops[0] = mop_block(d_blk);
            emit(c, jd2);
            mfunc_add_edge(c->mf, f_blk, d_blk);

            c->cur_mblk = d_blk;
            return;
        }

        case PIR_OP_ZEXT:
        case PIR_OP_SEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            PirScalarType src_sc = inst->operands[0].type.u.scalar;
            MVReg norm = src;
            if (src_sc == PIR_SCALAR_I8) {
                norm = (inst->op == PIR_OP_SEXT) ? emit_sext_sub(c, src, 8u, loc) : emit_zext_sub(c, src, 8u, loc);
            } else if (src_sc == PIR_SCALAR_I16) {
                norm = (inst->op == PIR_OP_SEXT) ? emit_sext_sub(c, src, 16u, loc) : emit_zext_sub(c, src, 16u, loc);
            }
            emit_copy(c, dst, norm, 4u, loc);
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

        case PIR_OP_FPEXT: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst cv = minst_make(MI_RV32_FCVT_D_S, 8u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);
            return;
        }

        case PIR_OP_FPTRUNC: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst cv = minst_make(MI_RV32_FCVT_S_D, 4u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);
            return;
        }

        case PIR_OP_SITOFP:
        case PIR_OP_UITOFP: {
            MVReg dst = c->val_vreg[inst->results[0]];
            bool is_s = (inst->op == PIR_OP_SITOFP);
            MVReg src = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->operands[0].type, is_s, loc);
            MInst cv = minst_make(is_s ? MI_RV32_FCVT_F_W : MI_RV32_FCVT_F_WU, rv32_vreg_bytes(inst->ty), loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);
            return;
        }

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg src = mat_op(c, &inst->operands[0], loc);
            uint8_t fw = rv32_vreg_bytes(inst->operands[0].type);
            bool is_s = (inst->op == PIR_OP_FPTOSI);
            PirScalarType dsc = inst->ty.u.scalar;

            if (inst->flags & PIR_FLAG_TRAP) {
                /* Promote f32 -> f64 for exact range comparisons against int bounds. */
                MVReg fd = src;
                if (fw == 4u) {
                    fd = new_fp(c, 8u);
                    MInst ext = minst_make(MI_RV32_FCVT_D_S, 8u, loc);
                    ext.op_count = 2u; ext.ops[0] = mop_vreg_def(fd); ext.ops[1] = mop_vreg_use(src);
                    emit(c, ext);
                }
                /* NaN check: feq fd, fd == 0 -> trap */
                MVReg ord = new_gp(c);
                MInst fe = minst_make(MI_RV32_FEQ, 8u, loc);
                fe.op_count = 3u; fe.ops[0] = mop_vreg_def(ord); fe.ops[1] = mop_vreg_use(fd); fe.ops[2] = mop_vreg_use(fd);
                emit(c, fe);
                emit_bcc_trap(c, MCOND_EQ, ord, MVREG_NONE, loc);

                /* Bounds check in f64 (truncation toward zero: valid iff lo_excl < x < hi_excl). */
                uint64_t lo_bits = 0u, hi_bits = 0u;
                if (is_s) {
                    if (dsc == PIR_SCALAR_I8)       { lo_bits = 0xC060200000000000ULL; /* -129.0 */ hi_bits = 0x4060000000000000ULL; /* 128.0 */ }
                    else if (dsc == PIR_SCALAR_I16) { lo_bits = 0xC0E0002000000000ULL; /* -32769.0 */ hi_bits = 0x40E0000000000000ULL; /* 32768.0 */ }
                    else                            { lo_bits = 0xC1E0000000200000ULL; /* -2^31 - 1 */ hi_bits = 0x41E0000000000000ULL; /* 2^31 */ }
                } else {
                    lo_bits = 0xBFF0000000000000ULL; /* -1.0 */
                    if (dsc == PIR_SCALAR_I8)       hi_bits = 0x4070000000000000ULL; /* 256.0 */
                    else if (dsc == PIR_SCALAR_I16) hi_bits = 0x40F0000000000000ULL; /* 65536.0 */
                    else                            hi_bits = 0x41F0000000000000ULL; /* 2^32 */
                }
                MVReg fl = new_fp(c, 8u), fh = new_fp(c, 8u);
                emit_imm_fp(c, fl, lo_bits, 8u, loc);
                emit_imm_fp(c, fh, hi_bits, 8u, loc);
                MVReg bad_lo = new_gp(c), bad_hi = new_gp(c);
                MInst cl = minst_make(MI_RV32_FLE, 8u, loc);
                cl.op_count = 3u; cl.ops[0] = mop_vreg_def(bad_lo); cl.ops[1] = mop_vreg_use(fd); cl.ops[2] = mop_vreg_use(fl);
                emit(c, cl);
                emit_bcc_trap(c, MCOND_NE, bad_lo, MVREG_NONE, loc);
                MInst ch = minst_make(MI_RV32_FLE, 8u, loc);
                ch.op_count = 3u; ch.ops[0] = mop_vreg_def(bad_hi); ch.ops[1] = mop_vreg_use(fh); ch.ops[2] = mop_vreg_use(fd);
                emit(c, ch);
                emit_bcc_trap(c, MCOND_NE, bad_hi, MVREG_NONE, loc);
            }

            MVReg raw = new_gp(c);
            MInst cv = minst_make(is_s ? MI_RV32_FCVT_W_F : MI_RV32_FCVT_WU_F, fw, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(raw); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);

            if (inst->flags & PIR_FLAG_UNSAFE) {
                emit_copy(c, dst, raw, 4u, loc);
                return;
            }

            /* RISC-V fcvt maps NaN to INT_MAX/UINT_MAX, whereas PIR [sat] requires NaN -> 0. */
            MVReg not_nan = new_gp(c), mask = new_gp(c), sat = new_gp(c);
            MInst fe = minst_make(MI_RV32_FEQ, fw, loc);
            fe.op_count = 3u; fe.ops[0] = mop_vreg_def(not_nan); fe.ops[1] = mop_vreg_use(src); fe.ops[2] = mop_vreg_use(src);
            emit(c, fe);
            /* mask = -not_nan (0 if NaN, -1 if non-NaN) */
            MInst neg = minst_make(MI_RV32_SUB, 4u, loc);
            neg.op_count = 3u; neg.ops[0] = mop_vreg_def(mask); neg.ops[1] = mop_preg_use(RV32_ZERO); neg.ops[2] = mop_vreg_use(not_nan);
            emit(c, neg);
            MInst an = minst_make(MI_RV32_AND, 4u, loc);
            an.op_count = 3u; an.ops[0] = mop_vreg_def(sat); an.ops[1] = mop_vreg_use(raw); an.ops[2] = mop_vreg_use(mask);
            emit(c, an);

            if (dsc == PIR_SCALAR_I8 || dsc == PIR_SCALAR_I16) {
                int32_t lo_i = is_s ? (dsc == PIR_SCALAR_I8 ? INT8_MIN : INT16_MIN) : 0;
                int32_t hi_i = is_s ? (dsc == PIR_SCALAR_I8 ? INT8_MAX : INT16_MAX)
                                    : (dsc == PIR_SCALAR_I8 ? 255 : 65535);
                MVReg vhi = new_gp(c);
                emit_li(c, vhi, hi_i, loc);

                /* Clamp sat to [lo_i, hi_i] using well-formed single-terminator MIR blocks. */
                uint32_t chk_hi    = mfunc_new_block(c->mf, "cl_hi");
                uint32_t cl_hi_set = mfunc_new_block(c->mf, "cl_hset");
                uint32_t cl_dn     = mfunc_new_block(c->mf, "cl_dn");
                emit_copy(c, dst, sat, 4u, loc);
                if (is_s) {
                    MVReg vlo = new_gp(c);
                    emit_li(c, vlo, lo_i, loc);
                    uint32_t cl_lo = mfunc_new_block(c->mf, "cl_lo");
                    MInst b1 = minst_make(MI_RV32_BCC, 4u, loc);
                    b1.op_count = 4u; b1.ops[0] = mop_cond(MCOND_GE);
                    b1.ops[1] = mop_vreg_use(sat); b1.ops[2] = mop_vreg_use(vlo); b1.ops[3] = mop_block(chk_hi);
                    emit(c, b1);
                    MInst jlo = minst_make(MI_RV32_J, 4u, loc);
                    jlo.op_count = 1u; jlo.ops[0] = mop_block(cl_lo);
                    emit(c, jlo);
                    mfunc_add_edge(c->mf, c->cur_mblk, chk_hi);
                    mfunc_add_edge(c->mf, c->cur_mblk, cl_lo);

                    c->cur_mblk = cl_lo;
                    emit_copy(c, dst, vlo, 4u, loc);
                    MInst j1 = minst_make(MI_RV32_J, 4u, loc);
                    j1.op_count = 1u; j1.ops[0] = mop_block(cl_dn);
                    emit(c, j1);
                    mfunc_add_edge(c->mf, cl_lo, cl_dn);
                } else {
                    MInst j0 = minst_make(MI_RV32_J, 4u, loc);
                    j0.op_count = 1u; j0.ops[0] = mop_block(chk_hi);
                    emit(c, j0);
                    mfunc_add_edge(c->mf, c->cur_mblk, chk_hi);
                }
                c->cur_mblk = chk_hi;
                MInst b2 = minst_make(MI_RV32_BCC, 4u, loc);
                b2.op_count = 4u; b2.ops[0] = mop_cond(is_s ? MCOND_LE : MCOND_LS);
                b2.ops[1] = mop_vreg_use(sat); b2.ops[2] = mop_vreg_use(vhi); b2.ops[3] = mop_block(cl_dn);
                emit(c, b2);
                MInst jhs = minst_make(MI_RV32_J, 4u, loc);
                jhs.op_count = 1u; jhs.ops[0] = mop_block(cl_hi_set);
                emit(c, jhs);
                mfunc_add_edge(c->mf, chk_hi, cl_dn);
                mfunc_add_edge(c->mf, chk_hi, cl_hi_set);

                c->cur_mblk = cl_hi_set;
                emit_copy(c, dst, vhi, 4u, loc);
                MInst j2 = minst_make(MI_RV32_J, 4u, loc);
                j2.op_count = 1u; j2.ops[0] = mop_block(cl_dn);
                emit(c, j2);
                mfunc_add_edge(c->mf, cl_hi_set, cl_dn);
                c->cur_mblk = cl_dn;
            } else {
                emit_copy(c, dst, sat, 4u, loc);
            }
            return;
        }

        case PIR_OP_BITCAST: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MRegClass dc = rv32_rclass(inst->ty);
            if (inst->operands[0].kind == PIR_OPER_IMM_INT && dc == MREG_CLASS_FP) {
                uint64_t raw = inst->operands[0].u.imm_int.is_negative
                               ? (UINT64_C(0) - inst->operands[0].u.imm_int.lo)
                               : inst->operands[0].u.imm_int.lo;
                emit_imm_fp(c, dst, raw, rv32_vreg_bytes(inst->ty), loc);
                return;
            }
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MRegClass sc = c->mf->vregs.data[src].rclass;
            if (dc == sc) {
                emit_copy(c, dst, src, rv32_vreg_bytes(inst->ty), loc);
            } else if (dc == MREG_CLASS_FP && sc == MREG_CLASS_GP) {
                MInst mv = minst_make(MI_RV32_FMV_W_X, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            } else {
                MInst mv = minst_make(MI_RV32_FMV_X_W, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            }
            return;
        }

        case PIR_OP_ALLOCA_PTR: {
            MVReg dst = c->val_vreg[inst->results[0]];
            uint64_t sz = inst->imm_u64 > 0u ? inst->imm_u64 : 1u;
            uint32_t al = inst->align > 0u ? inst->align : 1u;
            uint32_t sid = mfunc_add_slot(c->mf, sz, al, false);
            MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
            fa.op_count = 3u; fa.ops[0] = mop_vreg_def(dst); fa.ops[1] = mop_slot(sid); fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return;
        }

        case PIR_OP_ALLOCA_DYN: {
            c->mf->has_alloca_dyn = true;
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg sz  = mat_op(c, &inst->operands[0], loc);
            uint32_t al = inst->align > 16u ? inst->align : 16u;

            MVReg sp_cur = new_gp(c), sp_sub = new_gp(c), sp_new = new_gp(c);
            MInst gsp = minst_make(MI_RV32_MOV_SP, 4u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(sp_cur);
            emit(c, gsp);

            MInst sub = minst_make(MI_RV32_SUB, 4u, loc);
            sub.op_count = 3u; sub.ops[0] = mop_vreg_def(sp_sub);
            sub.ops[1] = mop_vreg_use(sp_cur); sub.ops[2] = mop_vreg_use(sz);
            emit(c, sub);

            int32_t mask = -(int32_t)al;
            if (fits_simm12(mask)) {
                MInst an = minst_make(MI_RV32_ANDI, 4u, loc);
                an.op_count = 3u; an.ops[0] = mop_vreg_def(sp_new);
                an.ops[1] = mop_vreg_use(sp_sub); an.ops[2] = mop_imm(mask);
                emit(c, an);
            } else {
                MVReg mr = new_gp(c);
                emit_li(c, mr, mask, loc);
                MInst an = minst_make(MI_RV32_AND, 4u, loc);
                an.op_count = 3u; an.ops[0] = mop_vreg_def(sp_new);
                an.ops[1] = mop_vreg_use(sp_sub); an.ops[2] = mop_vreg_use(mr);
                emit(c, an);
            }

            MInst ssp = minst_make(MI_RV32_MOV_SP, 4u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(sp_new);
            emit(c, ssp);

            emit_copy(c, dst, sp_new, 4u, loc);
            return;
        }

        case PIR_OP_STACKSAVE: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MInst gsp = minst_make(MI_RV32_MOV_SP, 4u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(dst);
            emit(c, gsp);
            return;
        }

        case PIR_OP_STACKRESTORE: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst ssp = minst_make(MI_RV32_MOV_SP, 4u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(src);
            emit(c, ssp);
            return;
        }

        case PIR_OP_PTR_ADD: {
            MVReg dst  = c->val_vreg[inst->results[0]];
            MVReg base = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                int32_t imm = (int32_t)eval_imm_u32(&inst->operands[1]);
                if (fits_simm12(imm)) {
                    MInst ai = minst_make(MI_RV32_ADDI, 4u, loc);
                    ai.op_count = 3u; ai.ops[0] = mop_vreg_def(dst);
                    ai.ops[1] = mop_vreg_use(base); ai.ops[2] = mop_imm(imm);
                    emit(c, ai);
                    return;
                }
            }
            MVReg off = mat_op(c, &inst->operands[1], loc);
            MInst ad = minst_make(MI_RV32_ADD, 4u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst);
            ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(off);
            emit(c, ad);
            return;
        }

        case PIR_OP_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = rv32_mem_bytes(inst->ty);
            bool is_fp = (rv32_rclass(inst->ty) == MREG_CLASS_FP);
            MInst ld = minst_make(is_fp ? MI_RV32_FLD : MI_RV32_LD, w, loc);
            ld.is_signed = false;
            ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
            emit(c, ld);
            return;
        }

        case PIR_OP_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = rv32_mem_bytes(inst->ty);
            bool is_fp = (rv32_rclass(inst->ty) == MREG_CLASS_FP);
            MInst st = minst_make(is_fp ? MI_RV32_FST : MI_RV32_ST, w, loc);
            st.op_count = 3u; st.ops[0] = mop_vreg_use(val);
            st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
            emit(c, st);
            return;
        }

        case PIR_OP_FENCE: {
            MInst f = minst_make(MI_RV32_FENCE, 4u, loc);
            f.aux_id = (uint32_t)inst->order;
            emit(c, f);
            return;
        }

        case PIR_OP_ATOMIC_LOAD: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            uint8_t w = rv32_mem_bytes(inst->ty);
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst f0 = minst_make(MI_RV32_FENCE, 4u, loc);
                f0.aux_id = (uint32_t)PIR_ORDER_SEQ_CST;
                emit(c, f0);
            }
            MInst ld = minst_make(MI_RV32_LD, w, loc);
            ld.is_signed = false;
            ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst);
            ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
            emit(c, ld);
            if (inst->order == PIR_ORDER_ACQUIRE || inst->order == PIR_ORDER_SEQ_CST) {
                MInst f1 = minst_make(MI_RV32_FENCE, 4u, loc);
                f1.aux_id = (uint32_t)PIR_ORDER_ACQUIRE;
                emit(c, f1);
            }
            return;
        }

        case PIR_OP_ATOMIC_STORE: {
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            uint8_t w = rv32_mem_bytes(inst->ty);
            if (inst->order == PIR_ORDER_RELEASE || inst->order == PIR_ORDER_SEQ_CST) {
                MInst f0 = minst_make(MI_RV32_FENCE, 4u, loc);
                f0.aux_id = (uint32_t)PIR_ORDER_RELEASE;
                emit(c, f0);
            }
            MInst st = minst_make(MI_RV32_ST, w, loc);
            st.op_count = 3u; st.ops[0] = mop_vreg_use(val);
            st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
            emit(c, st);
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst f1 = minst_make(MI_RV32_FENCE, 4u, loc);
                f1.aux_id = (uint32_t)PIR_ORDER_SEQ_CST;
                emit(c, f1);
            }
            return;
        }

        case PIR_OP_ATOMIC_RMW: {
            MVReg dst = c->val_vreg[inst->results[0]];
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);
            uint8_t w = rv32_mem_bytes(inst->ty);
            /* Pin operands to caller-saved GP regs (t0..t2) so they never spill into
             * t4/t5/t6 which MI_RV32_ATOMIC_RMW uses internally for its LR/SC loop. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = RV32_T0;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = RV32_T1;
            MVReg p_val = new_gp(c); c->mf->vregs.data[p_val].fixed_preg = RV32_T2;

            MInst cp1 = minst_make(MI_COPY, 4u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr);
            emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, 4u, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_val); cp2.ops[1] = mop_vreg_use(val);
            emit(c, cp2);

            MInst rmw = minst_make(MI_RV32_ATOMIC_RMW, w, loc);
            rmw.aux_id   = (uint32_t)inst->rmw_op;
            rmw.aux_id2  = (uint32_t)inst->order;
            rmw.op_count = 3u;
            rmw.ops[0]   = mop_vreg_def_ec(p_old);
            rmw.ops[1]   = mop_vreg_use(p_ptr);
            rmw.ops[2]   = mop_vreg_use(p_val);
            emit(c, rmw);

            MInst co = minst_make(MI_COPY, 4u, loc);
            co.op_count = 2u; co.ops[0] = mop_vreg_def(dst); co.ops[1] = mop_vreg_use(p_old);
            emit(c, co);
            return;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            MVReg old_v = c->val_vreg[inst->results[0]];
            MVReg ok_v  = c->val_vreg[inst->results[1]];
            MVReg ptr   = mat_op(c, &inst->operands[0], loc);
            MVReg exp_v = mat_op(c, &inst->operands[1], loc);
            MVReg des_v = mat_op(c, &inst->operands[2], loc);
            uint8_t w   = rv32_mem_bytes(inst->ty);
            /* Pin all 5 operands to distinct caller-saved GP regs (t0..t3, a0) so high
             * register pressure can never exceed 3 spill scratches or clobber t4..t6. */
            MVReg p_old = new_gp(c); c->mf->vregs.data[p_old].fixed_preg = RV32_T0;
            MVReg p_ok  = new_gp(c); c->mf->vregs.data[p_ok].fixed_preg  = RV32_T1;
            MVReg p_ptr = new_gp(c); c->mf->vregs.data[p_ptr].fixed_preg = RV32_T2;
            MVReg p_exp = new_gp(c); c->mf->vregs.data[p_exp].fixed_preg = RV32_T3;
            MVReg p_des = new_gp(c); c->mf->vregs.data[p_des].fixed_preg = RV32_A0;

            MInst cp1 = minst_make(MI_COPY, 4u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr);
            emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, 4u, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_exp); cp2.ops[1] = mop_vreg_use(exp_v);
            emit(c, cp2);
            MInst cp3 = minst_make(MI_COPY, 4u, loc);
            cp3.op_count = 2u; cp3.ops[0] = mop_vreg_def(p_des); cp3.ops[1] = mop_vreg_use(des_v);
            emit(c, cp3);

            MInst cas = minst_make(MI_RV32_ATOMIC_CAS, w, loc);
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
            co1.op_count = 2u; co1.ops[0] = mop_vreg_def(old_v); co1.ops[1] = mop_vreg_use(p_old);
            emit(c, co1);
            MInst co2 = minst_make(MI_COPY, 4u, loc);
            co2.op_count = 2u; co2.ops[0] = mop_vreg_def(ok_v); co2.ops[1] = mop_vreg_use(p_ok);
            emit(c, co2);
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
            uint32_t dst_b = c->pir_to_mblk[inst->targets[0].target];
            MInst j = minst_make(MI_RV32_J, 4u, loc);
            j.op_count = 1u; j.ops[0] = mop_block(dst_b);
            emit(c, j);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_b);
            return;
        }

        case PIR_OP_BR: {
            MVReg cond = normalize_int(c, mat_op(c, &inst->operands[0], loc),
                                       pir_type_scalar(PIR_SCALAR_I8), false, loc);
            uint32_t t_blk = resolve_edge_target(c, &inst->targets[0], loc);
            uint32_t f_blk = resolve_edge_target(c, &inst->targets[1], loc);
            MInst br = minst_make(MI_RV32_BCC, 4u, loc);
            br.op_count = 4u; br.ops[0] = mop_cond(MCOND_NE);
            br.ops[1] = mop_vreg_use(cond); br.ops[2] = mop_preg_use(RV32_ZERO);
            br.ops[3] = mop_block(t_blk);
            emit(c, br);
            MInst jf = minst_make(MI_RV32_J, 4u, loc);
            jf.op_count = 1u; jf.ops[0] = mop_block(f_blk);
            emit(c, jf);
            mfunc_add_edge(c->mf, c->cur_mblk, t_blk);
            mfunc_add_edge(c->mf, c->cur_mblk, f_blk);
            return;
        }

        case PIR_OP_SWITCH: {
            MVReg val = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, true, loc);
            uint32_t nc = inst->switch_case_count;
            uint32_t def_b = resolve_edge_target(c, &inst->targets[0], loc);

            /* Check if dense jump table is profitable (range <= 128 && nc >= 4). */
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
                c->mf->vregs.data[p_val].fixed_preg = RV32_T0;
                MInst cp = minst_make(MI_COPY, 4u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(p_val); cp.ops[1] = mop_vreg_use(val);
                emit(c, cp);
                MInst st = minst_make(MI_RV32_SWITCH_TBL, 4u, loc);
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

            /* Linear compare-and-branch chain. */
            for (uint32_t k = 0u; k < nc; ++k) {
                uint32_t raw = (uint32_t)(inst->switch_cases[k].is_negative
                                          ? (UINT64_C(0) - inst->switch_cases[k].case_lo)
                                          : inst->switch_cases[k].case_lo);
                int32_t sv = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? (int32_t)(int8_t)raw
                           : (inst->ty.u.scalar == PIR_SCALAR_I16) ? (int32_t)(int16_t)raw
                                                                   : (int32_t)raw;
                MVReg kv = new_gp(c);
                emit_li(c, kv, sv, loc);
                uint32_t tb = resolve_edge_target(c, &inst->switch_cases[k].target, loc);
                MInst br = minst_make(MI_RV32_BCC, 4u, loc);
                br.op_count = 4u; br.ops[0] = mop_cond(MCOND_EQ);
                br.ops[1] = mop_vreg_use(val); br.ops[2] = mop_vreg_use(kv); br.ops[3] = mop_block(tb);
                emit(c, br);
                mfunc_add_edge(c->mf, c->cur_mblk, tb);
            }
            MInst jd = minst_make(MI_RV32_J, 4u, loc);
            jd.op_count = 1u; jd.ops[0] = mop_block(def_b);
            emit(c, jd);
            mfunc_add_edge(c->mf, c->cur_mblk, def_b);
            return;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && c->multi_ret_slot != UINT32_MAX) {
                MVReg base = new_gp(c);
                MInst fa = minst_make(MI_FRAME_ADDR, 4u, loc);
                fa.op_count = 3u; fa.ops[0] = mop_vreg_def(base);
                fa.ops[1] = mop_slot(c->multi_ret_slot); fa.ops[2] = mop_imm(0);
                emit(c, fa);

                uint32_t rg = 0u, rf = 0u;
                uint64_t offs[4];
                emit_rv32_multi_ret_offsets(c, &c->fn->cabi_ret, offs);
                uint32_t agg_al = c->fn->cabi_ret.agg_align > 0u ? c->fn->cabi_ret.agg_align : 1u;
                MVReg loaded[4];
                MPReg dst_pr[4];
                uint8_t lw[4];
                for (uint32_t k = 0u; k < c->fn->cabi_ret.reg_count; ++k) {
                    PirType rt = c->fn->cabi_ret.regs[k];
                    bool is_fp = (rv32_rclass(rt) == MREG_CLASS_FP);
                    uint8_t mw = rv32_mem_bytes(rt);
                    lw[k] = rv32_vreg_bytes(rt);
                    loaded[k] = new_vreg_for_type(c, rt);
                    dst_pr[k] = is_fp ? RV32_FP(10u + rf++) : RV32_GP(10u + rg++);
                    bool unaligned = (mw > 1u) && ((offs[k] % mw) != 0u || agg_al < mw);
                    if (unaligned) {
                        uint32_t sl = get_f64_xfer_slot(c);
                        MVReg saddr = new_gp(c);
                        MInst sfa = minst_make(MI_FRAME_ADDR, 4u, loc);
                        sfa.op_count = 3u; sfa.ops[0] = mop_vreg_def(saddr);
                        sfa.ops[1] = mop_slot(sl); sfa.ops[2] = mop_imm(0);
                        emit(c, sfa);
                        emit_rv32_copy_bytes_mem(c, saddr, 0u, base, offs[k], mw, loc);
                        MInst ld = minst_make(MI_LOAD_SLOT, mw, loc);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(loaded[k]); ld.ops[1] = mop_slot(sl);
                        emit(c, ld);
                    } else {
                        MInst ld = minst_make(is_fp ? MI_RV32_FLD : MI_RV32_LD, mw, loc);
                        ld.is_signed = false;
                        ld.op_count = 3u; ld.ops[0] = mop_vreg_def(loaded[k]);
                        ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_imm((int64_t)offs[k]);
                        emit(c, ld);
                    }
                }
                for (uint32_t k = 0u; k < c->fn->cabi_ret.reg_count; ++k) {
                    MInst cp = minst_make(MI_COPY, lw[k], loc);
                    cp.op_count = 2u; cp.ops[0] = mop_preg_def(dst_pr[k]); cp.ops[1] = mop_vreg_use(loaded[k]);
                    emit(c, cp);
                }
            } else if (inst->operand_count == 1u) {
                MVReg rv = mat_op(c, &inst->operands[0], loc);
                if (inst->ty.kind == PIR_TYPE_SCALAR &&
                    (inst->ty.u.scalar == PIR_SCALAR_I8 || inst->ty.u.scalar == PIR_SCALAR_I16)) {
                    if ((c->fn->attrs & PIR_FATTR_RET_SIGNEXT) != 0u) {
                        rv = emit_normalize_subword(c, rv, inst->ty, true, loc);
                    } else if ((c->fn->attrs & PIR_FATTR_RET_ZEROEXT) != 0u) {
                        rv = emit_normalize_subword(c, rv, inst->ty, false, loc);
                    }
                }
                MPReg rpreg = (rv32_rclass(inst->ty) == MREG_CLASS_FP) ? RV32_FA0 : RV32_A0;
                MInst cp = minst_make(MI_COPY, rv32_vreg_bytes(inst->ty), loc);
                cp.op_count = 2u; cp.ops[0] = mop_preg_def(rpreg); cp.ops[1] = mop_vreg_use(rv);
                emit(c, cp);
            }
            MInst ret = minst_make(MI_RV32_RET, 4u, loc);
            emit(c, ret);
            return;
        }

        case PIR_OP_TRAP: {
            MInst tr = minst_make(MI_RV32_UNIMP, 4u, loc);
            emit(c, tr);
            return;
        }

        case PIR_OP_UNREACHABLE:
            return;

        /* Legalized away before RV32 ISel */
        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U:
        case PIR_OP_ADD_CARRY: case PIR_OP_SUB_BORROW:
        case PIR_OP_CLZ: case PIR_OP_CTZ: case PIR_OP_POPCNT: case PIR_OP_BSWAP:
        case PIR_OP_SPLAT: case PIR_OP_EXTRACT: case PIR_OP_INSERT: case PIR_OP_SHUFFLE:
        case PIR_OP_ALLOCA: case PIR_OP_FIELD_PTR: case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY: case PIR_OP_MEMMOVE: case PIR_OP_MEMSET:
        case PIR_OP_VA_ARG: case PIR_OP_VA_ARG_BUF: case PIR_OP_VA_COPY: case PIR_OP_VA_END:
        case PIR_OP_INVOKE: case PIR_OP_INVOKE_INDIRECT: case PIR_OP_RESUME:
            pir_diag_error(c->diag, inst->src_line, 0u,
                           "RV32 ISel: unexpected unlegalized opcode %d", (int)inst->op);
            return;
    }
}

/* =========================================================================
 * Public Entry Point
 * ========================================================================= */

bool pir_rv32_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                        const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);

    Rv32ISelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod            = mod;
    ctx.fn             = fn;
    ctx.target         = target;
    ctx.mf             = out_mf;
    ctx.diag           = diag;
    ctx.multi_ret_slot = UINT32_MAX;
    ctx.f64_xfer_slot  = UINT32_MAX;

    uint32_t nv = fn->values.len;
    uint32_t nb = fn->blocks.len;
    ctx.val_vreg    = nv ? (MVReg *)malloc(nv * sizeof(MVReg)) : NULL;
    ctx.pir_to_mblk = nb ? (uint32_t *)malloc(nb * sizeof(uint32_t)) : NULL;
    if ((nv && !ctx.val_vreg) || (nb && !ctx.pir_to_mblk)) abort();

    for (uint32_t v = 0u; v < nv; ++v) {
        ctx.val_vreg[v] = new_vreg_for_type(&ctx, fn->values.data[v].type);
    }

    /* Order blocks in RPO so linear-scan intervals follow execution flow. */
    PirCfg cfg;
    pir_cfg_build(&cfg, fn);
    uint32_t *blk_order = nb ? (uint32_t *)malloc(nb * sizeof(uint32_t)) : NULL;
    bool *seen = nb ? (bool *)calloc(nb, sizeof(bool)) : NULL;
    if (nb && (!blk_order || !seen)) abort();
    uint32_t ord_len = 0u;
    for (uint32_t i = 0u; i < cfg.rpo_len; ++i) {
        uint32_t b = cfg.rpo[i];
        if (b < nb && !seen[b]) {
            seen[b] = true;
            blk_order[ord_len++] = b;
        }
    }
    for (uint32_t b = 0u; b < nb; ++b) {
        if (!seen[b]) blk_order[ord_len++] = b;
    }
    free(seen);
    pir_cfg_destroy(&cfg);

    for (uint32_t i = 0u; i < ord_len; ++i) {
        uint32_t pb = blk_order[i];
        ctx.pir_to_mblk[pb] = mfunc_new_block(out_mf, fn->blocks.data[pb].label);
    }

    /* Emit prologue parameter copies in entry block. */
    if (nb > 0u) {
        ctx.cur_mblk = ctx.pir_to_mblk[0];
        bool has_multi_out = (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG);
        uint32_t p_start = 0u;
        if (has_multi_out && fn->param_count > 0u) {
            uint64_t rsz = fn->cabi_ret.agg_size > 0u ? fn->cabi_ret.agg_size : 8u;
            if (rsz < 8u) rsz = 8u;
            uint32_t ral = fn->cabi_ret.agg_align > 0u ? fn->cabi_ret.agg_align : 4u;
            if (ral < 4u) ral = 4u;
            ctx.multi_ret_slot = mfunc_add_slot(out_mf, rsz, ral, false);
            if (fn->params[0].val_id != PIR_INVALID_ID) {
                MInst fa = minst_make(MI_FRAME_ADDR, 4u, 0u);
                fa.op_count = 3u;
                fa.ops[0] = mop_vreg_def(ctx.val_vreg[fn->params[0].val_id]);
                fa.ops[1] = mop_slot(ctx.multi_ret_slot);
                fa.ops[2] = mop_imm(0);
                emit(&ctx, fa);
            }
            p_start = 1u;
        }

        uint32_t gp_idx = 0u;
        uint32_t fp_idx = 0u;
        uint64_t in_stk_off = 0u;

        for (uint32_t i = p_start; i < fn->param_count; ++i) {
            const PirParamDecl *pd = &fn->params[i];
            PirType pt = pd->type;
            MVReg dst = (pd->val_id != PIR_INVALID_ID) ? ctx.val_vreg[pd->val_id] : MVREG_NONE;

            if (pt.kind == PIR_TYPE_SCALAR && pt.u.scalar == PIR_SCALAR_F32) {
                if (fp_idx < 8u) {
                    MPReg src_p = RV32_FP(10u + fp_idx++);
                    if (dst != MVREG_NONE) {
                        out_mf->vregs.data[dst].hint_preg = src_p;
                        MInst cp = minst_make(MI_COPY, 4u, 0u);
                        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(src_p);
                        emit(&ctx, cp);
                    }
                } else if (gp_idx < 8u) {
                    MPReg src_p = RV32_GP(10u + gp_idx++);
                    if (dst != MVREG_NONE) {
                        MInst mv = minst_make(MI_RV32_FMV_W_X, 4u, 0u);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_preg_use(src_p);
                        emit(&ctx, mv);
                    }
                } else {
                    if (dst != MVREG_NONE) {
                        MInst ld = minst_make(MI_LOAD_INARG, 4u, 0u);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_imm((int64_t)in_stk_off);
                        emit(&ctx, ld);
                    }
                    in_stk_off += 4u;
                }
            } else if (pt.kind == PIR_TYPE_SCALAR && pt.u.scalar == PIR_SCALAR_F64) {
                if (fp_idx < 8u) {
                    MPReg src_p = RV32_FP(10u + fp_idx++);
                    if (dst != MVREG_NONE) {
                        out_mf->vregs.data[dst].hint_preg = src_p;
                        MInst cp = minst_make(MI_COPY, 8u, 0u);
                        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(src_p);
                        emit(&ctx, cp);
                    }
                } else if (gp_idx + 2u <= 8u) {
                    MPReg plo = RV32_GP(10u + gp_idx);
                    MPReg phi = RV32_GP(10u + gp_idx + 1u);
                    gp_idx += 2u;
                    if (dst != MVREG_NONE) {
                        uint32_t sl = get_f64_xfer_slot(&ctx);
                        MVReg addr = new_gp(&ctx);
                        MInst fa = minst_make(MI_FRAME_ADDR, 4u, 0u);
                        fa.op_count = 3u; fa.ops[0] = mop_vreg_def(addr); fa.ops[1] = mop_slot(sl); fa.ops[2] = mop_imm(0);
                        emit(&ctx, fa);
                        MInst s0 = minst_make(MI_RV32_ST, 4u, 0u);
                        s0.op_count = 3u; s0.ops[0] = mop_preg_use(plo); s0.ops[1] = mop_vreg_use(addr); s0.ops[2] = mop_imm(0);
                        emit(&ctx, s0);
                        MInst s1 = minst_make(MI_RV32_ST, 4u, 0u);
                        s1.op_count = 3u; s1.ops[0] = mop_preg_use(phi); s1.ops[1] = mop_vreg_use(addr); s1.ops[2] = mop_imm(4);
                        emit(&ctx, s1);
                        MInst ld = minst_make(MI_LOAD_SLOT, 8u, 0u);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_slot(sl);
                        emit(&ctx, ld);
                    }
                } else if (gp_idx == 7u) {
                    /* Split across a7 (lo) and caller sp+0 (hi). */
                    gp_idx = 8u;
                    if (dst != MVREG_NONE) {
                        uint32_t sl = get_f64_xfer_slot(&ctx);
                        MVReg hi_v = new_gp(&ctx);
                        MInst lhi = minst_make(MI_LOAD_INARG, 4u, 0u);
                        lhi.op_count = 2u; lhi.ops[0] = mop_vreg_def(hi_v); lhi.ops[1] = mop_imm((int64_t)in_stk_off);
                        emit(&ctx, lhi);
                        MVReg addr = new_gp(&ctx);
                        MInst fa = minst_make(MI_FRAME_ADDR, 4u, 0u);
                        fa.op_count = 3u; fa.ops[0] = mop_vreg_def(addr); fa.ops[1] = mop_slot(sl); fa.ops[2] = mop_imm(0);
                        emit(&ctx, fa);
                        MInst s0 = minst_make(MI_RV32_ST, 4u, 0u);
                        s0.op_count = 3u; s0.ops[0] = mop_preg_use(RV32_A7); s0.ops[1] = mop_vreg_use(addr); s0.ops[2] = mop_imm(0);
                        emit(&ctx, s0);
                        MInst s1 = minst_make(MI_RV32_ST, 4u, 0u);
                        s1.op_count = 3u; s1.ops[0] = mop_vreg_use(hi_v); s1.ops[1] = mop_vreg_use(addr); s1.ops[2] = mop_imm(4);
                        emit(&ctx, s1);
                        MInst ld = minst_make(MI_LOAD_SLOT, 8u, 0u);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_slot(sl);
                        emit(&ctx, ld);
                    }
                    in_stk_off += 4u;
                } else {
                    in_stk_off = align_up_u64(in_stk_off, 8u);
                    if (dst != MVREG_NONE) {
                        MInst ld = minst_make(MI_LOAD_INARG, 8u, 0u);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_imm((int64_t)in_stk_off);
                        emit(&ctx, ld);
                    }
                    in_stk_off += 8u;
                }
            } else {
                if (gp_idx < 8u && !pd->abi_force_stack) {
                    MPReg src_p = RV32_GP(10u + gp_idx++);
                    if (dst != MVREG_NONE) {
                        out_mf->vregs.data[dst].hint_preg = src_p;
                        MInst cp = minst_make(MI_COPY, 4u, 0u);
                        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(src_p);
                        emit(&ctx, cp);
                    }
                } else {
                    uint64_t al = (pd->abi_stack_align > 4u) ? (uint64_t)pd->abi_stack_align : 4u;
                    in_stk_off = align_up_u64(in_stk_off, al);
                    if (dst != MVREG_NONE) {
                        MInst ld = minst_make(MI_LOAD_INARG, 4u, 0u);
                        ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_imm((int64_t)in_stk_off);
                        emit(&ctx, ld);
                    }
                    in_stk_off += 4u;
                }
            }
        }

        out_mf->va_fixed_gp          = gp_idx;
        out_mf->va_fixed_fp          = fp_idx;
        out_mf->va_fixed_stack_bytes = in_stk_off;
    }

    /* Translate every PIR block in RPO order. */
    for (uint32_t i = 0u; i < ord_len; ++i) {
        uint32_t pb = blk_order[i];
        const PirBlock *blk = &fn->blocks.data[pb];
        ctx.cur_mblk = ctx.pir_to_mblk[pb];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            isel_inst(&ctx, &fn->insts.data[blk->insts.data[j]]);
            if (diag->error_count > 0u) break;
        }
        if (diag->error_count > 0u) break;
    }

    free(blk_order);
    free(ctx.val_vreg);
    free(ctx.pir_to_mblk);
    return diag->error_count == 0u;
}
