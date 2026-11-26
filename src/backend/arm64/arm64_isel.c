/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

#include "analysis/cfg.h"
#include "lower/lower.h"

/* =========================================================================
 * Type & Register Helpers
 * ========================================================================= */

static MRegClass pir_type_rclass(PirType ty) {
    if (ty.kind == PIR_TYPE_VECTOR) return MREG_CLASS_FP;
    if (ty.kind == PIR_TYPE_SCALAR &&
        (ty.u.scalar == PIR_SCALAR_F32 || ty.u.scalar == PIR_SCALAR_F64)) {
        return MREG_CLASS_FP;
    }
    return MREG_CLASS_GP;
}

/* Storage width in register/spill slot: GP uses 4 or 8; FP uses 4, 8, or 16. */
static uint8_t pir_type_vreg_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_VECTOR) {
        return (uint8_t)(pir_vec_bits(ty.u.vec) / 8u);
    }
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:
            case PIR_SCALAR_I16:
            case PIR_SCALAR_I32:
            case PIR_SCALAR_F32:
                return 4u;
            case PIR_SCALAR_I64:
            case PIR_SCALAR_F64:
            case PIR_SCALAR_PTR:
                return 8u;
            case PIR_SCALAR_VOID:
            case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST:
                return 8u;
        }
    }
    return 8u;
}

/* Exact memory byte width: 1, 2, 4, 8, or 16. */
static uint8_t pir_type_mem_bytes(PirType ty) {
    if (ty.kind == PIR_TYPE_VECTOR) {
        return (uint8_t)(pir_vec_bits(ty.u.vec) / 8u);
    }
    if (ty.kind == PIR_TYPE_SCALAR) {
        switch (ty.u.scalar) {
            case PIR_SCALAR_I8:  return 1u;
            case PIR_SCALAR_I16: return 2u;
            case PIR_SCALAR_I32:
            case PIR_SCALAR_F32: return 4u;
            case PIR_SCALAR_I64:
            case PIR_SCALAR_F64:
            case PIR_SCALAR_PTR: return 8u;
            case PIR_SCALAR_VOID:
            case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST: return 0u;
        }
    }
    return 8u;
}

static MVecArr pir_vec_to_arr(PirVecType v) {
    switch (v) {
        case PIR_VEC_V8I8:   return MARR_8B;
        case PIR_VEC_V4I16:  return MARR_4H;
        case PIR_VEC_V2I32:
        case PIR_VEC_V2F32:  return MARR_2S;
        case PIR_VEC_V16I8:  return MARR_16B;
        case PIR_VEC_V8I16:  return MARR_8H;
        case PIR_VEC_V4I32:
        case PIR_VEC_V4F32:  return MARR_4S;
        case PIR_VEC_V2I64:
        case PIR_VEC_V2F64:  return MARR_2D;
        case PIR_VEC_NONE:
        case PIR_VEC_V32I8: case PIR_VEC_V16I16: case PIR_VEC_V8I32:
        case PIR_VEC_V4I64: case PIR_VEC_V8F32:  case PIR_VEC_V4F64:
            return MARR_NONE;
    }
    return MARR_NONE;
}

static uint64_t eval_imm_u64(const PirOperand *op) {
    uint64_t v = op->u.imm_int.lo;
    if (op->u.imm_int.is_negative) {
        v = UINT64_C(0) - v;
    }
    return v;
}

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
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

    MVReg               *val_vreg;     /* PirValueId -> MVReg */
    uint32_t            *pir_to_mblk;  /* PirBlockId -> MBlock id */
    uint32_t            *val_uses;     /* PirValueId -> use count */
    bool                *inst_fused;   /* PirInstId -> skipped due to fusion */
    PirInstId            last_flags_cmp_id; /* Fused cmp/fcmp currently live in NZCV */
    uint32_t             cur_mblk;     /* Active MBlock being populated */
    uint32_t             multi_ret_slot; /* Local buffer slot for PIR_CABI_RET_MULTI_REG */
} ISelCtx;

static MBlock *cur_blk(ISelCtx *c) {
    return &c->mf->blocks.data[c->cur_mblk];
}

static void emit(ISelCtx *c, MInst ins) {
    mblock_push(cur_blk(c), ins);
}

static MVReg new_gp(ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, bytes <= 4u ? 4u : 8u);
}

static MVReg new_fp(ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_FP, bytes);
}

static MVReg new_vreg_for_type(ISelCtx *c, PirType ty) {
    return mfunc_new_vreg(c->mf, pir_type_rclass(ty), pir_type_vreg_bytes(ty));
}

/* Emit movz/movn + movk sequence to load an arbitrary 32/64-bit constant into dst. */
static void emit_imm_gp(ISelCtx *c, MVReg dst, uint64_t val, uint8_t width, uint32_t loc) {
    if (width <= 4u) {
        val &= 0xFFFFFFFFu;
        uint16_t lo = (uint16_t)(val & 0xFFFFu);
        uint16_t hi = (uint16_t)((val >> 16u) & 0xFFFFu);
        MInst m = minst_make(MI_A64_MOVZ, 4u, loc);
        m.aux_width = 0u;
        m.op_count = 2u; m.ops[0] = mop_vreg_def(dst); m.ops[1] = mop_imm(lo);
        emit(c, m);
        if (hi != 0u) {
            MInst k = minst_make(MI_A64_MOVK, 4u, loc);
            k.aux_width = 16u;
            k.op_count = 2u; k.ops[0] = mop_vreg_defuse(dst); k.ops[1] = mop_imm(hi);
            emit(c, k);
        }
        return;
    }
    uint16_t chunks[4] = {
        (uint16_t)(val & 0xFFFFu),
        (uint16_t)((val >> 16u) & 0xFFFFu),
        (uint16_t)((val >> 32u) & 0xFFFFu),
        (uint16_t)((val >> 48u) & 0xFFFFu)
    };
    uint32_t zcnt = 0u, fcnt = 0u;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (chunks[i] == 0u) zcnt++;
        if (chunks[i] == 0xFFFFu) fcnt++;
    }
    bool first = true;
    if (fcnt > zcnt) {
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (chunks[i] == 0xFFFFu && !(first && i == 3u)) continue;
            if (first) {
                MInst m = minst_make(MI_A64_MOVN, 8u, loc);
                m.aux_width = (uint8_t)(i * 16u);
                m.op_count = 2u; m.ops[0] = mop_vreg_def(dst); m.ops[1] = mop_imm((uint16_t)(~chunks[i]));
                emit(c, m);
                first = false;
            } else {
                MInst k = minst_make(MI_A64_MOVK, 8u, loc);
                k.aux_width = (uint8_t)(i * 16u);
                k.op_count = 2u; k.ops[0] = mop_vreg_defuse(dst); k.ops[1] = mop_imm(chunks[i]);
                emit(c, k);
            }
        }
    } else {
        for (uint32_t i = 0u; i < 4u; ++i) {
            if (chunks[i] == 0u && !(first && i == 3u)) continue;
            if (first) {
                MInst m = minst_make(MI_A64_MOVZ, 8u, loc);
                m.aux_width = (uint8_t)(i * 16u);
                m.op_count = 2u; m.ops[0] = mop_vreg_def(dst); m.ops[1] = mop_imm(chunks[i]);
                emit(c, m);
                first = false;
            } else {
                MInst k = minst_make(MI_A64_MOVK, 8u, loc);
                k.aux_width = (uint8_t)(i * 16u);
                k.op_count = 2u; k.ops[0] = mop_vreg_defuse(dst); k.ops[1] = mop_imm(chunks[i]);
                emit(c, k);
            }
        }
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
    /* Unknown symbol: assume extern GOT */
    *out_use_got = true;
}

static void emit_sym_addr(ISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false, use_got = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls, &use_got);

    MVReg base = (offset != 0) ? new_gp(c, 8u) : dst;
    if (is_tls) {
        if (c->target->os == PIR_OS_MACOS) {
            /* Darwin TLV thunk call returns thread variable address in x0 and clobbers caller-saved regs! */
            c->mf->has_calls = true;
            MInst t = minst_make(MI_A64_TLS_ADDR, 8u, loc);
            t.is_call  = true;
            t.op_count = 2u;
            t.ops[0]   = mop_preg_def(A64_GP(0u));
            t.ops[1]   = mop_sym(name, 0, false, true, use_got);
            emit(c, t);
            MInst cp = minst_make(MI_COPY, 8u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(base); cp.ops[1] = mop_preg_use(A64_GP(0u));
            emit(c, cp);
        } else {
            /* Linux local-exec TLS: mrs base, TPIDR_EL0; add :tprel_hi12:; add :tprel_lo12_nc: */
            MInst t = minst_make(MI_A64_TLS_ADDR, 8u, loc);
            t.op_count = 2u;
            t.ops[0]   = mop_vreg_def(base);
            t.ops[1]   = mop_sym(name, 0, false, true, false);
            emit(c, t);
        }
    } else {
        MInst a = minst_make(MI_A64_SYM_ADDR, 8u, loc);
        a.op_count = 2u;
        a.ops[0]   = mop_vreg_def(base);
        a.ops[1]   = mop_sym(name, 0, is_func, false, use_got);
        emit(c, a);
    }

    if (offset != 0) {
        if (offset > 0 && offset <= 4095) {
            MInst ad = minst_make(MI_A64_ADD_RI, 8u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_imm(offset);
            emit(c, ad);
        } else if (offset < 0 && -offset <= 4095) {
            MInst sb = minst_make(MI_A64_SUB_RI, 8u, loc);
            sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst); sb.ops[1] = mop_vreg_use(base);
            sb.ops[2] = mop_imm(-offset);
            emit(c, sb);
        } else {
            MVReg off_r = new_gp(c, 8u);
            emit_imm_gp(c, off_r, (uint64_t)offset, 8u, loc);
            MInst ad = minst_make(MI_A64_ADD_RR, 8u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst); ad.ops[1] = mop_vreg_use(base);
            ad.ops[2] = mop_vreg_use(off_r);
            emit(c, ad);
        }
    }
}

static MVReg mat_op(ISelCtx *c, const PirOperand *op, uint32_t loc) {
    switch (op->kind) {
        case PIR_OPER_VAL:
            return c->val_vreg[op->u.val];
        case PIR_OPER_IMM_INT: {
            uint8_t w = pir_type_vreg_bytes(op->type);
            MVReg r = new_gp(c, w);
            emit_imm_gp(c, r, eval_imm_u64(op), w, loc);
            return r;
        }
        case PIR_OPER_IMM_FLOAT: {
            uint8_t w = pir_type_vreg_bytes(op->type);
            MVReg gp = new_gp(c, w);
            MVReg fp = new_fp(c, w);
            emit_imm_gp(c, gp, op->u.imm_float.bits, w, loc);
            MInst fm = minst_make(MI_A64_FMOV_G2F, w, loc);
            fm.op_count = 2u; fm.ops[0] = mop_vreg_def(fp); fm.ops[1] = mop_vreg_use(gp);
            emit(c, fm);
            return fp;
        }
        case PIR_OPER_SYM: {
            MVReg r = new_gp(c, 8u);
            emit_sym_addr(c, r, op->u.sym.name, op->u.sym.offset, loc);
            return r;
        }
        case PIR_OPER_INVOKE_RET:
        case PIR_OPER_INVOKE_EXN:
            abort();
    }
    abort();
}

/* Normalize sub-word integer (i8/i16) into a 32-bit value (zero- or sign-extended). */
static MVReg normalize_int(ISelCtx *c, MVReg src, PirType ty, bool is_signed, uint32_t loc) {
    if (ty.kind != PIR_TYPE_SCALAR) return src;
    if (ty.u.scalar == PIR_SCALAR_I8) {
        MVReg r = new_gp(c, 4u);
        MInst e = minst_make(is_signed ? MI_A64_SXTB : MI_A64_UXTB, 4u, loc);
        e.op_count = 2u; e.ops[0] = mop_vreg_def(r); e.ops[1] = mop_vreg_use(src);
        emit(c, e);
        return r;
    }
    if (ty.u.scalar == PIR_SCALAR_I16) {
        MVReg r = new_gp(c, 4u);
        MInst e = minst_make(is_signed ? MI_A64_SXTH : MI_A64_UXTH, 4u, loc);
        e.op_count = 2u; e.ops[0] = mop_vreg_def(r); e.ops[1] = mop_vreg_use(src);
        emit(c, e);
        return r;
    }
    return src;
}

/* Emit a conditional trap: if cond holds on NZCV flags, execute udf #0. */
static void emit_cond_trap(ISelCtx *c, MCond trap_cond, uint32_t loc) {
    /* Create a tiny inline trap block + continuation block so branch target is a clean MBlock. */
    uint32_t trap_blk = mfunc_new_block(c->mf, "trap");
    uint32_t ok_blk   = mfunc_new_block(c->mf, "ok");

    MInst bc = minst_make(MI_A64_BCOND, 4u, loc);
    bc.op_count = 2u; bc.ops[0] = mop_cond(trap_cond); bc.ops[1] = mop_block(trap_blk);
    emit(c, bc);

    MInst b_ok = minst_make(MI_A64_B, 4u, loc);
    b_ok.op_count = 1u; b_ok.ops[0] = mop_block(ok_blk);
    emit(c, b_ok);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_blk);
    mfunc_add_edge(c->mf, c->cur_mblk, ok_blk);

    MInst udf = minst_make(MI_A64_UDF, 4u, loc);
    mblock_push(&c->mf->blocks.data[trap_blk], udf);

    c->cur_mblk = ok_blk;
}

/* =========================================================================
 * Edge Parallel Copy Helper
 * ========================================================================= */

static uint32_t build_edge_pcopy(ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    const PirBlock *dst_pblk = &c->fn->blocks.data[bt->target];
    if (bt->arg_count == 0u || dst_pblk->params.len == 0u) {
        return UINT32_MAX;
    }
    MPCopyEntry *entries = (MPCopyEntry *)calloc(bt->arg_count, sizeof(MPCopyEntry));
    if (!entries) abort();
    uint32_t cnt = 0u;
    for (uint32_t i = 0u; i < bt->arg_count; ++i) {
        PirValueId pv = dst_pblk->params.data[i];
        PirType pty   = c->fn->values.data[pv].type;
        entries[cnt].dst       = c->val_vreg[pv];
        entries[cnt].rclass    = pir_type_rclass(pty);
        entries[cnt].byte_size = pir_type_vreg_bytes(pty);
        if (bt->args[i].kind == PIR_OPER_VAL) {
            entries[cnt].src_is_imm = false;
            entries[cnt].src        = c->val_vreg[bt->args[i].u.val];
        } else if (bt->args[i].kind == PIR_OPER_IMM_INT) {
            entries[cnt].src_is_imm = true;
            entries[cnt].imm_bits   = eval_imm_u64(&bt->args[i]);
        } else if (bt->args[i].kind == PIR_OPER_IMM_FLOAT) {
            entries[cnt].src_is_imm = true;
            entries[cnt].imm_bits   = bt->args[i].u.imm_float.bits;
        } else {
            /* Symbol operand on edge: materialize in current block before copy */
            entries[cnt].src_is_imm = false;
            entries[cnt].src        = mat_op(c, &bt->args[i], loc);
        }
        cnt++;
    }
    uint32_t id = mfunc_add_pcopy(c->mf, entries, cnt);
    free(entries);
    return id;
}

/* Ensure an edge target has a trampoline MBlock if it carries block arguments
 * on a multi-successor branch (br / switch). */
static uint32_t prepare_branch_edge(ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
    uint32_t dst_mblk = c->pir_to_mblk[bt->target];
    if (bt->arg_count == 0u) return dst_mblk;

    uint32_t saved_blk = c->cur_mblk;
    uint32_t tramp = mfunc_new_block(c->mf, "edge");
    c->cur_mblk = tramp;
    uint32_t pc_id = build_edge_pcopy(c, bt, loc);
    if (pc_id != UINT32_MAX) {
        MInst pc = minst_make(MI_PCOPY, 0u, loc);
        pc.aux_id = pc_id;
        emit(c, pc);
    }
    MInst jmp = minst_make(MI_A64_B, 4u, loc);
    jmp.op_count = 1u; jmp.ops[0] = mop_block(dst_mblk);
    emit(c, jmp);
    mfunc_add_edge(c->mf, tramp, dst_mblk);
    c->cur_mblk = saved_blk;
    return tramp;
}

/* =========================================================================
 * Comparison Helpers (Scalar Int & Float + Branch Fusion)
 * ========================================================================= */

static MCond int_cmp_to_cond(PirOpcode op) {
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

static bool int_cmp_is_signed(PirOpcode op) {
    return op == PIR_OP_CMP_LT_S || op == PIR_OP_CMP_LE_S ||
           op == PIR_OP_CMP_GT_S || op == PIR_OP_CMP_GE_S;
}

static bool fcmp_single_cond(PirOpcode op, MCond *out) {
    if (op == PIR_OP_FCMP_OEQ) { *out = MCOND_EQ; return true; }
    if (op == PIR_OP_FCMP_OGT) { *out = MCOND_GT; return true; }
    if (op == PIR_OP_FCMP_OGE) { *out = MCOND_GE; return true; }
    if (op == PIR_OP_FCMP_OLT) { *out = MCOND_MI; return true; }
    if (op == PIR_OP_FCMP_OLE) { *out = MCOND_LS; return true; }
    if (op == PIR_OP_FCMP_UNE) { *out = MCOND_NE; return true; }
    if (op == PIR_OP_FCMP_ULT) { *out = MCOND_LT; return true; }
    if (op == PIR_OP_FCMP_ULE) { *out = MCOND_LE; return true; }
    if (op == PIR_OP_FCMP_UGT) { *out = MCOND_HI; return true; }
    if (op == PIR_OP_FCMP_UGE) { *out = MCOND_PL; return true; }
    return false;
}

static bool is_already_zext_subword(const PirFunction *fn, const PirOperand *op, PirType ty) {
    if (op->kind != PIR_OPER_VAL || ty.kind != PIR_TYPE_SCALAR) return false;
    PirScalarType sc = ty.u.scalar;
    if (sc != PIR_SCALAR_I8 && sc != PIR_SCALAR_I16) return false;
    PirValueId v = op->u.val;
    if (v >= fn->values.len) return false;
    PirInstId did = fn->values.data[v].def_inst;
    if (did == PIR_INVALID_ID) return false;
    const PirInst *d = &fn->insts.data[did];
    if ((d->op == PIR_OP_LOAD || d->op == PIR_OP_ATOMIC_LOAD) && pir_type_eq(d->ty, ty)) {
        return true; /* ARM64 ldrb/ldrh/ldarb/ldarh zero-extend to 32/64 bits */
    }
    if (sc == PIR_SCALAR_I8 && d->ty.kind == PIR_TYPE_SCALAR &&
        ((d->op >= PIR_OP_CMP_EQ && d->op <= PIR_OP_CMP_GE_U) ||
         (d->op >= PIR_OP_FCMP_OEQ && d->op <= PIR_OP_FCMP_UGE))) {
        return true; /* ARM64 cset writes 0 or 1 into 32/64-bit register */
    }
    if (sc == PIR_SCALAR_I16 && d->op == PIR_OP_ZEXT && d->operand_count == 1u &&
        d->operands[0].type.kind == PIR_TYPE_SCALAR &&
        d->operands[0].type.u.scalar == PIR_SCALAR_I8) {
        return true; /* uxtb already zero-extends to 32/64 bits */
    }
    if (d->op == PIR_OP_TRUNC && d->operand_count == 1u &&
        d->operands[0].kind == PIR_OPER_VAL &&
        d->operands[0].type.kind == PIR_TYPE_SCALAR &&
        d->operands[0].type.u.scalar == PIR_SCALAR_I32) {
        PirValueId xv = d->operands[0].u.val;
        if (xv < fn->values.len && fn->values.data[xv].def_inst != PIR_INVALID_ID) {
            const PirInst *x = &fn->insts.data[fn->values.data[xv].def_inst];
            uint64_t lim = (sc == PIR_SCALAR_I8) ? 0xFFu : 0xFFFFu;
            uint64_t sh_min = (sc == PIR_SCALAR_I8) ? 24u : 16u;
            if (x->op == PIR_OP_AND && x->operand_count == 2u) {
                for (uint32_t k = 0u; k < 2u; ++k) {
                    if (x->operands[k].kind == PIR_OPER_IMM_INT &&
                        !x->operands[k].u.imm_int.is_negative &&
                        x->operands[k].u.imm_int.hi == 0u &&
                        x->operands[k].u.imm_int.lo <= lim) {
                        return true;
                    }
                }
            }
            if (x->op == PIR_OP_SHR && x->operand_count == 2u &&
                x->operands[1].kind == PIR_OPER_IMM_INT &&
                !x->operands[1].u.imm_int.is_negative &&
                x->operands[1].u.imm_int.hi == 0u &&
                x->operands[1].u.imm_int.lo >= sh_min &&
                x->operands[1].u.imm_int.lo < 32u) {
                return true;
            }
        }
    }
    return false;
}

static void emit_scalar_int_cmp_flags(ISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_signed = int_cmp_is_signed(inst->op);
    uint8_t w = pir_type_vreg_bytes(inst->ty);
    MVReg lhs_raw = mat_op(c, &inst->operands[0], loc);
    MVReg lhs = (!is_signed && is_already_zext_subword(c->fn, &inst->operands[0], inst->ty))
                ? lhs_raw
                : normalize_int(c, lhs_raw, inst->ty, is_signed, loc);

    /* Use CMP_RI whenever normalized rhs immediate fits ARM64 unsigned 12-bit [0, 4095]. */
    if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
        if (w == 8u || (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_I32)) {
            if (!inst->operands[1].u.imm_int.is_negative && inst->operands[1].u.imm_int.lo <= 4095u) {
                MInst cmp = minst_make(MI_A64_CMP_RI, w, loc);
                cmp.op_count = 2u;
                cmp.ops[0] = mop_vreg_use(lhs);
                cmp.ops[1] = mop_imm((int64_t)inst->operands[1].u.imm_int.lo);
                emit(c, cmp);
                return;
            }
        } else if (inst->ty.kind == PIR_TYPE_SCALAR &&
                   (inst->ty.u.scalar == PIR_SCALAR_I8 || inst->ty.u.scalar == PIR_SCALAR_I16)) {
            uint64_t raw = eval_imm_u64(&inst->operands[1]);
            int64_t norm = (inst->ty.u.scalar == PIR_SCALAR_I8)
                           ? (is_signed ? (int64_t)(int8_t)raw : (int64_t)(uint8_t)raw)
                           : (is_signed ? (int64_t)(int16_t)raw : (int64_t)(uint16_t)raw);
            if (norm >= 0 && norm <= 4095) {
                MInst cmp = minst_make(MI_A64_CMP_RI, 4u, loc);
                cmp.op_count = 2u;
                cmp.ops[0] = mop_vreg_use(lhs);
                cmp.ops[1] = mop_imm(norm);
                emit(c, cmp);
                return;
            }
        }
    }
    MVReg rhs_raw = mat_op(c, &inst->operands[1], loc);
    MVReg rhs = (!is_signed && is_already_zext_subword(c->fn, &inst->operands[1], inst->ty))
                ? rhs_raw
                : normalize_int(c, rhs_raw, inst->ty, is_signed, loc);
    MInst cmp = minst_make(MI_A64_CMP_RR, w, loc);
    cmp.op_count = 2u;
    cmp.ops[0] = mop_vreg_use(lhs);
    cmp.ops[1] = mop_vreg_use(rhs);
    emit(c, cmp);
}

/* =========================================================================
 * Vector (NEON) Instruction Selection Helpers
 * ========================================================================= */

static void emit_vec_lane_extract(ISelCtx *c, MVReg dst, MVReg vec, PirVecType vt,
                                  uint32_t lane, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    MVecArr arr = pir_vec_to_arr(vt);
    if (pir_scalar_is_float(el)) {
        MInst ex = minst_make(MI_A64_VFMOV_LANE, pir_type_vreg_bytes(pir_type_scalar(el)), loc);
        ex.arr = arr; ex.aux_width = (uint8_t)lane;
        ex.op_count = 2u; ex.ops[0] = mop_vreg_def(dst); ex.ops[1] = mop_vreg_use(vec);
        emit(c, ex);
    } else {
        MInst ex = minst_make(MI_A64_VUMOV, pir_type_vreg_bytes(pir_type_scalar(el)), loc);
        ex.arr = arr; ex.aux_width = (uint8_t)lane;
        ex.op_count = 2u; ex.ops[0] = mop_vreg_def(dst); ex.ops[1] = mop_vreg_use(vec);
        emit(c, ex);
    }
}

static void emit_vec_lane_insert(ISelCtx *c, MVReg dst, MVReg base_vec, MVReg scalar,
                                 PirVecType vt, uint32_t lane, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
    MVecArr arr = pir_vec_to_arr(vt);
    /* Copy base_vec -> dst first, then ins in-place into dst */
    MInst cp = minst_make(MI_COPY, vb, loc);
    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(base_vec);
    emit(c, cp);
    if (pir_scalar_is_float(el)) {
        MInst ins = minst_make(MI_A64_VINS_LANE, vb, loc);
        ins.arr = arr; ins.aux_width = (uint8_t)lane; ins.aux_lane2 = 0u;
        ins.op_count = 2u; ins.ops[0] = mop_vreg_defuse(dst); ins.ops[1] = mop_vreg_use(scalar);
        emit(c, ins);
    } else {
        MInst ins = minst_make(MI_A64_VINS_GP, vb, loc);
        ins.arr = arr; ins.aux_width = (uint8_t)lane;
        ins.op_count = 2u; ins.ops[0] = mop_vreg_defuse(dst); ins.ops[1] = mop_vreg_use(scalar);
        emit(c, ins);
    }
}

static void emit_vec_select(ISelCtx *c, MVReg dst, MVReg mask, MVReg tv, MVReg fv,
                            uint8_t vbytes, uint32_t loc) {
    /* Copy fv -> dst, then bit dst, tv, mask (writes dst directly without a second copy!). */
    MInst cp = minst_make(MI_COPY, vbytes, loc);
    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(fv);
    emit(c, cp);
    MInst bit = minst_make(MI_A64_VBIT, vbytes, loc);
    bit.arr = (vbytes == 8u) ? MARR_8B : MARR_16B;
    bit.op_count = 3u;
    bit.ops[0] = mop_vreg_defuse(dst);
    bit.ops[1] = mop_vreg_use(tv);
    bit.ops[2] = mop_vreg_use(mask);
    emit(c, bit);
}

static bool val_has_fast_fp_origin(const PirFunction *fn, PirBlockId bid, PirValueId v, uint32_t depth) {
    if (depth == 0u || v >= fn->values.len) return false;
    const PirValueDef *vd = &fn->values.data[v];
    if (vd->def_inst == PIR_INVALID_ID || vd->def_block != bid) return false;
    const PirInst *ins = &fn->insts.data[vd->def_inst];
    if ((ins->flags & PIR_FLAG_FAST) != 0u) return true;
    if (ins->op == PIR_OP_SELECT && ins->operand_count == 3u) {
        if (ins->operands[1].kind == PIR_OPER_VAL &&
            val_has_fast_fp_origin(fn, bid, ins->operands[1].u.val, depth - 1u)) return true;
        if (ins->operands[2].kind == PIR_OPER_VAL &&
            val_has_fast_fp_origin(fn, bid, ins->operands[2].u.val, depth - 1u)) return true;
    }
    return false;
}

static bool is_arm64_bitmask_imm(uint64_t val, uint8_t w) {
    uint32_t bits = (w <= 4u) ? 32u : 64u;
    uint64_t full = (bits == 64u) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
    uint64_t v = val & full;
    if (v == 0u || v == full) return false;
    /* Check if v is a single contiguous run of 1-bits (possibly shifted): always valid in ARM64! */
    uint64_t lowest = v & (0u - v);
    uint64_t sum = v + lowest;
    return (sum & (sum - 1u)) == 0u;
}

static bool try_match_madd(const PirFunction *fn, const uint32_t *val_uses,
                           PirBlockId bid, const PirInst *add_ins,
                           PirInstId *out_mul_id, PirOperand *out_m0,
                           PirOperand *out_m1, PirOperand *out_ra) {
    if (add_ins->op != PIR_OP_ADD || add_ins->ty.kind != PIR_TYPE_SCALAR) return false;
    uint32_t bits = pir_scalar_bits(add_ins->ty.u.scalar, 8u);
    if (bits != 32u && bits != 64u) return false;
    if ((add_ins->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) return false;
    for (int side = 0; side < 2; ++side) {
        PirOperand mo = add_ins->operands[side];
        if (mo.kind != PIR_OPER_VAL) continue;
        PirValueId mv = mo.u.val;
        if (mv >= fn->values.len || val_uses[mv] != 1u) continue;
        const PirValueDef *vd = &fn->values.data[mv];
        if (vd->def_inst == PIR_INVALID_ID || vd->def_block != bid) continue;
        const PirInst *mi = &fn->insts.data[vd->def_inst];
        if (mi->op != PIR_OP_MUL || !pir_type_eq(mi->ty, add_ins->ty)) continue;
        if ((mi->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) continue;
        *out_mul_id = vd->def_inst;
        *out_m0     = mi->operands[0];
        *out_m1     = mi->operands[1];
        *out_ra     = add_ins->operands[1 - side];
        return true;
    }
    return false;
}

static bool pir_oper_val_eq(PirOperand a, PirOperand b) {
    return a.kind == PIR_OPER_VAL && b.kind == PIR_OPER_VAL && a.u.val == b.u.val;
}

/* Match (x >> r_amt) |/^/+ (x << (bits - r_amt)) -> ARM64 ror (immediate),
 * or    (x >> n)     |     (x << (bits - n))     -> ARM64 rorv (register). */
static bool try_match_ror(const PirFunction *fn, const uint32_t *val_uses,
                          PirBlockId bid, const PirInst *comb,
                          PirInstId *out_shr_id, PirInstId *out_shl_id,
                          PirOperand *out_x, bool *out_is_imm,
                          uint32_t *out_imm, PirOperand *out_reg_n) {
    if (comb->op != PIR_OP_OR && comb->op != PIR_OP_XOR && comb->op != PIR_OP_ADD) return false;
    if (comb->ty.kind != PIR_TYPE_SCALAR) return false;
    uint32_t bits = pir_scalar_bits(comb->ty.u.scalar, 8u);
    if (bits != 32u && bits != 64u) return false;
    if ((comb->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) return false;
    if (comb->operands[0].kind != PIR_OPER_VAL || comb->operands[1].kind != PIR_OPER_VAL) return false;

    PirValueId v0 = comb->operands[0].u.val;
    PirValueId v1 = comb->operands[1].u.val;
    if (v0 == v1 || v0 >= fn->values.len || v1 >= fn->values.len) return false;
    if (val_uses[v0] != 1u || val_uses[v1] != 1u) return false;

    const PirValueDef *d0 = &fn->values.data[v0];
    const PirValueDef *d1 = &fn->values.data[v1];
    if (d0->def_inst == PIR_INVALID_ID || d1->def_inst == PIR_INVALID_ID) return false;
    if (d0->def_block != bid || d1->def_block != bid) return false;

    const PirInst *i0 = &fn->insts.data[d0->def_inst];
    const PirInst *i1 = &fn->insts.data[d1->def_inst];
    PirInstId shr_id, shl_id;
    const PirInst *shr_ins, *shl_ins;
    if (i0->op == PIR_OP_SHR && i1->op == PIR_OP_SHL) {
        shr_id = d0->def_inst; shl_id = d1->def_inst;
        shr_ins = i0; shl_ins = i1;
    } else if (i0->op == PIR_OP_SHL && i1->op == PIR_OP_SHR) {
        shl_id = d0->def_inst; shr_id = d1->def_inst;
        shl_ins = i0; shr_ins = i1;
    } else {
        return false;
    }
    if (!pir_type_eq(shr_ins->ty, comb->ty) || !pir_type_eq(shl_ins->ty, comb->ty)) return false;
    if (((shr_ins->flags | shl_ins->flags) & PIR_FLAG_TRAP) != 0u) return false;
    if (!pir_oper_val_eq(shr_ins->operands[0], shl_ins->operands[0])) return false;

    if (shr_ins->operands[1].kind == PIR_OPER_IMM_INT &&
        shl_ins->operands[1].kind == PIR_OPER_IMM_INT) {
        uint64_t ra = eval_imm_u64(&shr_ins->operands[1]);
        uint64_t la = eval_imm_u64(&shl_ins->operands[1]);
        if (ra >= 1u && ra < bits && la >= 1u && la < bits && (ra + la) == bits) {
            *out_shr_id = shr_id;
            *out_shl_id = shl_id;
            *out_x      = shr_ins->operands[0];
            *out_is_imm = true;
            *out_imm    = (uint32_t)ra;
            return true;
        }
    }

    if (comb->op == PIR_OP_OR &&
        shr_ins->operands[1].kind == PIR_OPER_VAL &&
        shl_ins->operands[1].kind == PIR_OPER_VAL) {
        PirValueId nv = shr_ins->operands[1].u.val;
        PirValueId lv = shl_ins->operands[1].u.val;
        if (lv < fn->values.len) {
            const PirValueDef *lvd = &fn->values.data[lv];
            if (lvd->def_inst != PIR_INVALID_ID) {
                const PirInst *sub_ins = &fn->insts.data[lvd->def_inst];
                if (sub_ins->op == PIR_OP_SUB && pir_type_eq(sub_ins->ty, comb->ty) &&
                    sub_ins->operands[0].kind == PIR_OPER_IMM_INT &&
                    sub_ins->operands[1].kind == PIR_OPER_VAL &&
                    sub_ins->operands[1].u.val == nv) {
                    uint64_t lhs = eval_imm_u64(&sub_ins->operands[0]);
                    if (lhs == bits || lhs == 0u) {
                        *out_shr_id = shr_id;
                        *out_shl_id = shl_id;
                        *out_x      = shr_ins->operands[0];
                        *out_is_imm = false;
                        *out_imm    = 0u;
                        *out_reg_n  = shr_ins->operands[1];
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

/* Match select.vNf32/vNf64 (fcmp_{ogt,oge,olt,ole}(a, b)), tv, fv -> NEON fmin/fmax when [fast]-eligible. */
static bool try_match_vec_fminmax(const PirFunction *fn, const uint32_t *val_uses,
                                  PirBlockId bid, const PirInst *sel,
                                  PirInstId *out_fcmp_id, MOpcode *out_mop,
                                  PirOperand *out_a, PirOperand *out_b) {
    if (sel->op != PIR_OP_SELECT || sel->ty.kind != PIR_TYPE_VECTOR) return false;
    PirScalarType el = pir_vec_elem_scalar(sel->ty.u.vec);
    if (!pir_scalar_is_float(el)) return false;
    if (sel->operands[0].kind != PIR_OPER_VAL) return false;
    PirValueId cv = sel->operands[0].u.val;
    if (cv >= fn->values.len || val_uses[cv] != 1u) return false;
    const PirValueDef *vd = &fn->values.data[cv];
    if (vd->def_inst == PIR_INVALID_ID || vd->def_block != bid) return false;
    const PirInst *fc = &fn->insts.data[vd->def_inst];
    if (fc->ty.kind != PIR_TYPE_VECTOR || fc->ty.u.vec != sel->ty.u.vec) return false;

    bool is_gt = (fc->op == PIR_OP_FCMP_OGT || fc->op == PIR_OP_FCMP_OGE ||
                  fc->op == PIR_OP_FCMP_UGT || fc->op == PIR_OP_FCMP_UGE);
    bool is_lt = (fc->op == PIR_OP_FCMP_OLT || fc->op == PIR_OP_FCMP_OLE ||
                  fc->op == PIR_OP_FCMP_ULT || fc->op == PIR_OP_FCMP_ULE);
    if (!is_gt && !is_lt) return false;

    PirOperand a = fc->operands[0];
    PirOperand b = fc->operands[1];
    PirOperand tv = sel->operands[1];
    PirOperand fv = sel->operands[2];
    bool same_order = pir_oper_val_eq(tv, a) && pir_oper_val_eq(fv, b);
    bool swap_order = pir_oper_val_eq(tv, b) && pir_oper_val_eq(fv, a);
    if (!same_order && !swap_order) return false;

    /* Only fold to fmin/fmax when fast-math permits NaN/signed-zero relaxation. */
    bool fast_ok = ((sel->flags & PIR_FLAG_FAST) != 0u) ||
                   ((fc->flags & PIR_FLAG_FAST) != 0u) ||
                   (a.kind == PIR_OPER_VAL && val_has_fast_fp_origin(fn, bid, a.u.val, 4u)) ||
                   (b.kind == PIR_OPER_VAL && val_has_fast_fp_origin(fn, bid, b.u.val, 4u));
    if (!fast_ok) return false;

    /* (a > b ? a : b) or (a < b ? b : a) => fmax; (a > b ? b : a) or (a < b ? a : b) => fmin */
    bool is_max = (is_gt && same_order) || (is_lt && swap_order);
    *out_fcmp_id = vd->def_inst;
    *out_mop     = is_max ? MI_A64_VFMAX : MI_A64_VFMIN;
    *out_a       = a;
    *out_b       = b;
    return true;
}

static void emit_vec_cmp(ISelCtx *c, PirOpcode op, MVReg dst, MVReg a, MVReg b,
                         PirVecType vt, uint32_t loc) {
    MVecArr arr = pir_vec_to_arr(vt);
    uint8_t vb  = (uint8_t)(pir_vec_bits(vt) / 8u);
    if (op == PIR_OP_FCMP_ONE || op == PIR_OP_FCMP_UEQ) {
        MVReg t1 = new_fp(c, vb);
        MVReg t2 = new_fp(c, vb);
        MInst c1 = minst_make(MI_A64_VFCMGT, vb, loc);
        c1.arr = arr; c1.op_count = 3u;
        c1.ops[0] = mop_vreg_def(t1); c1.ops[1] = mop_vreg_use(a); c1.ops[2] = mop_vreg_use(b);
        emit(c, c1);
        MInst c2 = minst_make(MI_A64_VFCMGT, vb, loc);
        c2.arr = arr; c2.op_count = 3u;
        c2.ops[0] = mop_vreg_def(t2); c2.ops[1] = mop_vreg_use(b); c2.ops[2] = mop_vreg_use(a);
        emit(c, c2);
        MVReg tor = (op == PIR_OP_FCMP_UEQ) ? new_fp(c, vb) : dst;
        MInst orr = minst_make(MI_A64_VORR, vb, loc);
        orr.arr = (vb == 8u) ? MARR_8B : MARR_16B;
        orr.op_count = 3u;
        orr.ops[0] = mop_vreg_def(tor); orr.ops[1] = mop_vreg_use(t1); orr.ops[2] = mop_vreg_use(t2);
        emit(c, orr);
        if (op == PIR_OP_FCMP_UEQ) {
            MInst inv = minst_make(MI_A64_VMVN, vb, loc);
            inv.arr = (vb == 8u) ? MARR_8B : MARR_16B;
            inv.op_count = 2u; inv.ops[0] = mop_vreg_def(dst); inv.ops[1] = mop_vreg_use(tor);
            emit(c, inv);
        }
        return;
    }

    MOpcode mop = MI_A64_VCMEQ;
    bool swap = false, invert = false;
    if      (op == PIR_OP_CMP_EQ)   { mop = MI_A64_VCMEQ; }
    else if (op == PIR_OP_CMP_NE)   { mop = MI_A64_VCMEQ; invert = true; }
    else if (op == PIR_OP_CMP_GT_S) { mop = MI_A64_VCMGT; }
    else if (op == PIR_OP_CMP_GE_S) { mop = MI_A64_VCMGE; }
    else if (op == PIR_OP_CMP_LT_S) { mop = MI_A64_VCMGT; swap = true; }
    else if (op == PIR_OP_CMP_LE_S) { mop = MI_A64_VCMGE; swap = true; }
    else if (op == PIR_OP_CMP_GT_U) { mop = MI_A64_VCMHI; }
    else if (op == PIR_OP_CMP_GE_U) { mop = MI_A64_VCMHS; }
    else if (op == PIR_OP_CMP_LT_U) { mop = MI_A64_VCMHI; swap = true; }
    else if (op == PIR_OP_CMP_LE_U) { mop = MI_A64_VCMHS; swap = true; }
    else if (op == PIR_OP_FCMP_OEQ) { mop = MI_A64_VFCMEQ; }
    else if (op == PIR_OP_FCMP_OGT) { mop = MI_A64_VFCMGT; }
    else if (op == PIR_OP_FCMP_OGE) { mop = MI_A64_VFCMGE; }
    else if (op == PIR_OP_FCMP_OLT) { mop = MI_A64_VFCMGT; swap = true; }
    else if (op == PIR_OP_FCMP_OLE) { mop = MI_A64_VFCMGE; swap = true; }
    else if (op == PIR_OP_FCMP_UNE) { mop = MI_A64_VFCMEQ; invert = true; }
    else if (op == PIR_OP_FCMP_ULT) { mop = MI_A64_VFCMGE; invert = true; }
    else if (op == PIR_OP_FCMP_ULE) { mop = MI_A64_VFCMGT; invert = true; }
    else if (op == PIR_OP_FCMP_UGT) { mop = MI_A64_VFCMGE; swap = true; invert = true; }
    else if (op == PIR_OP_FCMP_UGE) { mop = MI_A64_VFCMGT; swap = true; invert = true; }

    MVReg out_r = invert ? new_fp(c, vb) : dst;
    MInst cmp = minst_make(mop, vb, loc);
    cmp.arr = arr; cmp.op_count = 3u;
    cmp.ops[0] = mop_vreg_def(out_r);
    cmp.ops[1] = mop_vreg_use(swap ? b : a);
    cmp.ops[2] = mop_vreg_use(swap ? a : b);
    emit(c, cmp);
    if (invert) {
        MInst inv = minst_make(MI_A64_VMVN, vb, loc);
        inv.arr = (vb == 8u) ? MARR_8B : MARR_16B;
        inv.op_count = 2u; inv.ops[0] = mop_vreg_def(dst); inv.ops[1] = mop_vreg_use(out_r);
        emit(c, inv);
    }
}

/* =========================================================================
 * Scalar Arithmetic / Div / Shift / Ovf / Carry / Bitops / Conversions
 * ========================================================================= */

static void emit_scalar_div_rem_regs(ISelCtx *c, PirOpcode op, PirType ty, uint32_t flags,
                                     MVReg a_raw, MVReg b_raw, MVReg dst, uint32_t loc) {
    bool is_signed = (op == PIR_OP_DIV_S || op == PIR_OP_REM_S);
    bool is_rem    = (op == PIR_OP_REM_S || op == PIR_OP_REM_U);
    uint8_t w      = pir_type_vreg_bytes(ty);
    MVReg a = normalize_int(c, a_raw, ty, is_signed, loc);
    MVReg b = normalize_int(c, b_raw, ty, is_signed, loc);

    if ((flags & PIR_FLAG_UNSAFE) == 0u) {
        /* Trap on divisor == 0 (default, [trap], and [wrap] all trap on /0). */
        MInst cz = minst_make(MI_A64_CMP_RI, w, loc);
        cz.op_count = 2u; cz.ops[0] = mop_vreg_use(b); cz.ops[1] = mop_imm(0);
        emit(c, cz);
        emit_cond_trap(c, MCOND_EQ, loc);

        if (is_signed && (flags & PIR_FLAG_WRAP) == 0u) {
            /* Trap on INT_MIN / -1 (default and [trap]). */
            uint32_t bits = pir_scalar_bits(ty.u.scalar, 8u);
            uint64_t int_min = (bits == 64u) ? (UINT64_C(1) << 63u)
                                             : (uint64_t)(UINT32_C(0) - (UINT32_C(1) << (bits - 1u)));
            MVReg m1 = new_gp(c, w);
            emit_imm_gp(c, m1, UINT64_MAX, w, loc);
            MInst cm1 = minst_make(MI_A64_CMP_RR, w, loc);
            cm1.op_count = 2u; cm1.ops[0] = mop_vreg_use(b); cm1.ops[1] = mop_vreg_use(m1);
            emit(c, cm1);
            MVReg is_m1 = new_gp(c, 4u);
            MInst s1 = minst_make(MI_A64_CSET, 4u, loc);
            s1.op_count = 2u; s1.ops[0] = mop_vreg_def(is_m1); s1.ops[1] = mop_cond(MCOND_EQ);
            emit(c, s1);

            MVReg imin = new_gp(c, w);
            emit_imm_gp(c, imin, int_min, w, loc);
            MInst cmin = minst_make(MI_A64_CMP_RR, w, loc);
            cmin.op_count = 2u; cmin.ops[0] = mop_vreg_use(a); cmin.ops[1] = mop_vreg_use(imin);
            emit(c, cmin);
            MVReg is_min = new_gp(c, 4u);
            MInst s2 = minst_make(MI_A64_CSET, 4u, loc);
            s2.op_count = 2u; s2.ops[0] = mop_vreg_def(is_min); s2.ops[1] = mop_cond(MCOND_EQ);
            emit(c, s2);

            MVReg both = new_gp(c, 4u);
            MInst an = minst_make(MI_A64_AND_RR, 4u, loc);
            an.op_count = 3u; an.ops[0] = mop_vreg_def(both); an.ops[1] = mop_vreg_use(is_m1); an.ops[2] = mop_vreg_use(is_min);
            emit(c, an);
            MInst tb = minst_make(MI_A64_CMP_RI, 4u, loc);
            tb.op_count = 2u; tb.ops[0] = mop_vreg_use(both); tb.ops[1] = mop_imm(0);
            emit(c, tb);
            emit_cond_trap(c, MCOND_NE, loc);
        }
    }

    MVReg q = is_rem ? new_gp(c, w) : dst;
    MInst dv = minst_make(is_signed ? MI_A64_SDIV : MI_A64_UDIV, w, loc);
    dv.op_count = 3u; dv.ops[0] = mop_vreg_def(q); dv.ops[1] = mop_vreg_use(a); dv.ops[2] = mop_vreg_use(b);
    emit(c, dv);
    if (is_rem) {
        MInst ms = minst_make(MI_A64_MSUB, w, loc);
        ms.op_count = 4u;
        ms.ops[0] = mop_vreg_def(dst);
        ms.ops[1] = mop_vreg_use(q);
        ms.ops[2] = mop_vreg_use(b);
        ms.ops[3] = mop_vreg_use(a);
        emit(c, ms);
    }
}

static void emit_scalar_div_rem(ISelCtx *c, const PirInst *inst, MVReg dst, uint32_t loc) {
    MVReg a = mat_op(c, &inst->operands[0], loc);
    MVReg b = mat_op(c, &inst->operands[1], loc);
    emit_scalar_div_rem_regs(c, inst->op, inst->ty, inst->flags, a, b, dst, loc);
}

static uint64_t fptoi_bound_bits(double v, uint8_t fw) {
    if (fw == 4u) {
        float fv = (float)v;
        uint32_t b = 0u;
        memcpy(&b, &fv, 4u);
        return (uint64_t)b;
    }
    uint64_t b = 0u;
    memcpy(&b, &v, 8u);
    return b;
}

static void emit_scalar_fptoi_regs(ISelCtx *c, bool is_signed, PirType src_ty, PirType dst_ty,
                                   uint32_t flags, MVReg src, MVReg dst, uint32_t loc) {
    uint8_t fw    = pir_type_vreg_bytes(src_ty);
    uint32_t bits = pir_scalar_bits(dst_ty.u.scalar, 8u);
    uint8_t dw    = pir_type_vreg_bytes(dst_ty);
    bool do_sat   = (flags & (PIR_FLAG_TRAP | PIR_FLAG_UNSAFE)) == 0u;

    if ((flags & PIR_FLAG_TRAP) != 0u) {
        /* Trap on NaN (fcmp src, src -> vs) and out-of-range (including +-Inf). */
        MInst fn = minst_make(MI_A64_FCMP, fw, loc);
        fn.op_count = 2u; fn.ops[0] = mop_vreg_use(src); fn.ops[1] = mop_vreg_use(src);
        emit(c, fn);
        emit_cond_trap(c, MCOND_VS, loc);

        /* Upper bound: trap if src >= hi_lim (2^(bits-1) signed, 2^bits unsigned). */
        double hi_d = is_signed ? ((bits == 64u) ? 9223372036854775808.0 : (double)(UINT64_C(1) << (bits - 1u)))
                                : ((bits == 64u) ? 18446744073709551616.0 : (double)(UINT64_C(1) << bits));
        MVReg hi_gp = new_gp(c, fw), hi_fp = new_fp(c, fw);
        emit_imm_gp(c, hi_gp, fptoi_bound_bits(hi_d, fw), fw, loc);
        MInst fm_hi = minst_make(MI_A64_FMOV_G2F, fw, loc);
        fm_hi.op_count = 2u; fm_hi.ops[0] = mop_vreg_def(hi_fp); fm_hi.ops[1] = mop_vreg_use(hi_gp);
        emit(c, fm_hi);
        MInst cm_hi = minst_make(MI_A64_FCMP, fw, loc);
        cm_hi.op_count = 2u; cm_hi.ops[0] = mop_vreg_use(src); cm_hi.ops[1] = mop_vreg_use(hi_fp);
        emit(c, cm_hi);
        emit_cond_trap(c, MCOND_GE, loc);

        /* Lower bound: trunc(src) < min_val. */
        double lo_d = -1.0;
        MCond lo_cond = MCOND_LE;
        if (is_signed) {
            if (bits == 64u) {
                lo_d = -9223372036854775808.0;
                lo_cond = MCOND_LT;
            } else if (bits == 32u && fw == 4u) {
                lo_d = -2147483648.0;
                lo_cond = MCOND_LT;
            } else {
                lo_d = -(double)((UINT64_C(1) << (bits - 1u)) + 1u);
                lo_cond = MCOND_LE;
            }
        }
        MVReg lo_gp = new_gp(c, fw), lo_fp = new_fp(c, fw);
        emit_imm_gp(c, lo_gp, fptoi_bound_bits(lo_d, fw), fw, loc);
        MInst fm_lo = minst_make(MI_A64_FMOV_G2F, fw, loc);
        fm_lo.op_count = 2u; fm_lo.ops[0] = mop_vreg_def(lo_fp); fm_lo.ops[1] = mop_vreg_use(lo_gp);
        emit(c, fm_lo);
        MInst cm_lo = minst_make(MI_A64_FCMP, fw, loc);
        cm_lo.op_count = 2u; cm_lo.ops[0] = mop_vreg_use(src); cm_lo.ops[1] = mop_vreg_use(lo_fp);
        emit(c, cm_lo);
        emit_cond_trap(c, lo_cond, loc);
    }

    /* ARM64 fcvtzs/fcvtzu natively saturate at 32/64-bit bounds and map NaN -> 0! */
    MVReg raw = (bits < 32u && do_sat) ? new_gp(c, 4u) : dst;
    MInst cv = minst_make(is_signed ? MI_A64_FCVTZS : MI_A64_FCVTZU, dw, loc);
    cv.aux_width = fw;
    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(raw); cv.ops[1] = mop_vreg_use(src);
    emit(c, cv);

    if (bits < 32u && do_sat) {
        /* Clamp 32-bit result to [lo_bound, hi_bound] for i8/i16 [sat]. */
        int64_t lo_b = is_signed ? -(int64_t)(1ULL << (bits - 1u)) : 0;
        int64_t hi_b = is_signed ? (int64_t)((1ULL << (bits - 1u)) - 1ULL) : (int64_t)((1ULL << bits) - 1ULL);
        MVReg r_hi = new_gp(c, 4u), c1 = new_gp(c, 4u);
        emit_imm_gp(c, r_hi, (uint64_t)hi_b, 4u, loc);
        MInst cm1 = minst_make(MI_A64_CMP_RR, 4u, loc);
        cm1.op_count = 2u; cm1.ops[0] = mop_vreg_use(raw); cm1.ops[1] = mop_vreg_use(r_hi);
        emit(c, cm1);
        MInst s1 = minst_make(MI_A64_CSEL, 4u, loc);
        s1.op_count = 4u; s1.ops[0] = mop_vreg_def(c1); s1.ops[1] = mop_vreg_use(r_hi);
        s1.ops[2] = mop_vreg_use(raw); s1.ops[3] = mop_cond(is_signed ? MCOND_GT : MCOND_HI);
        emit(c, s1);

        if (is_signed) {
            MVReg r_lo = new_gp(c, 4u);
            emit_imm_gp(c, r_lo, (uint64_t)lo_b, 4u, loc);
            MInst cm2 = minst_make(MI_A64_CMP_RR, 4u, loc);
            cm2.op_count = 2u; cm2.ops[0] = mop_vreg_use(c1); cm2.ops[1] = mop_vreg_use(r_lo);
            emit(c, cm2);
            MInst s2 = minst_make(MI_A64_CSEL, 4u, loc);
            s2.op_count = 4u; s2.ops[0] = mop_vreg_def(dst); s2.ops[1] = mop_vreg_use(r_lo);
            s2.ops[2] = mop_vreg_use(c1); s2.ops[3] = mop_cond(MCOND_LT);
            emit(c, s2);
        } else {
            MInst cp = minst_make(MI_COPY, 4u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(c1);
            emit(c, cp);
        }
    }
}

static void emit_vec_splat_scalar(ISelCtx *c, MVReg dst, MVReg scalar, PirVecType vt, uint32_t loc) {
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
    MOpcode mop = pir_scalar_is_float(pir_vec_elem_scalar(vt)) ? MI_A64_VDUP_LANE : MI_A64_VDUP_GP;
    MInst dp = minst_make(mop, vb, loc);
    dp.arr = pir_vec_to_arr(vt);
    dp.op_count = 2u; dp.ops[0] = mop_vreg_def(dst); dp.ops[1] = mop_vreg_use(scalar);
    emit(c, dp);
}

static bool op_is_uimm12(const PirOperand *op, int64_t *out_imm) {
    if (op->kind == PIR_OPER_IMM_INT && !op->u.imm_int.is_negative &&
        op->u.imm_int.hi == 0u && op->u.imm_int.lo <= 4095u) {
        *out_imm = (int64_t)op->u.imm_int.lo;
        return true;
    }
    return false;
}

static MCond emit_scalar_ovf_regs(ISelCtx *c, PirOpcode op, PirType ty,
                                  MVReg a_raw, MVReg b_raw, MVReg dst_val,
                                  MVReg dst_ovf, uint32_t loc, bool emit_cset) {
    bool is_signed = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_SUB_OVF_S || op == PIR_OP_MUL_OVF_S);
    uint32_t bits  = pir_scalar_bits(ty.u.scalar, 8u);
    uint8_t w      = pir_type_vreg_bytes(ty);
    MCond ovf_cond = MCOND_NE;

    if (bits < 32u) {
        MVReg a = normalize_int(c, a_raw, ty, is_signed, loc);
        MVReg b = normalize_int(c, b_raw, ty, is_signed, loc);
        MOpcode alu = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U) ? MI_A64_ADD_RR
                    : (op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) ? MI_A64_SUB_RR
                                                                         : MI_A64_MUL;
        MInst m = minst_make(alu, 4u, loc);
        m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a); m.ops[2] = mop_vreg_use(b);
        emit(c, m);
        MVReg trunc_back = normalize_int(c, dst_val, ty, is_signed, loc);
        MInst cmp = minst_make(MI_A64_CMP_RR, 4u, loc);
        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(dst_val); cmp.ops[1] = mop_vreg_use(trunc_back);
        emit(c, cmp);
        ovf_cond = MCOND_NE;
    } else if (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U) {
        MInst m = minst_make(MI_A64_ADDS_RR, w, loc);
        m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_vreg_use(b_raw);
        emit(c, m);
        ovf_cond = is_signed ? MCOND_VS : MCOND_CS;
    } else if (op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) {
        MInst m = minst_make(MI_A64_SUBS_RR, w, loc);
        m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_vreg_use(b_raw);
        emit(c, m);
        ovf_cond = is_signed ? MCOND_VS : MCOND_CC;
    } else if (bits == 32u) {
        MVReg wide = new_gp(c, 8u);
        MInst mw = minst_make(is_signed ? MI_A64_SMULL : MI_A64_UMULL, 8u, loc);
        mw.op_count = 3u; mw.ops[0] = mop_vreg_def(wide); mw.ops[1] = mop_vreg_use(a_raw); mw.ops[2] = mop_vreg_use(b_raw);
        emit(c, mw);
        MInst lo = minst_make(MI_A64_MOV_RR, 4u, loc);
        lo.op_count = 2u; lo.ops[0] = mop_vreg_def(dst_val); lo.ops[1] = mop_vreg_use(wide);
        emit(c, lo);
        if (is_signed) {
            MInst cmp = minst_make(MI_A64_CMP_SXTW, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(wide); cmp.ops[1] = mop_vreg_use(dst_val);
            emit(c, cmp);
        } else {
            MVReg hi = new_gp(c, 8u);
            MInst sh = minst_make(MI_A64_LSR_RI, 8u, loc);
            sh.op_count = 3u; sh.ops[0] = mop_vreg_def(hi); sh.ops[1] = mop_vreg_use(wide); sh.ops[2] = mop_imm(32);
            emit(c, sh);
            MInst cmp = minst_make(MI_A64_CMP_RI, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_imm(0);
            emit(c, cmp);
        }
        ovf_cond = MCOND_NE;
    } else {
        MVReg hi = new_gp(c, 8u);
        MInst mh = minst_make(is_signed ? MI_A64_SMULH : MI_A64_UMULH, 8u, loc);
        mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi); mh.ops[1] = mop_vreg_use(a_raw); mh.ops[2] = mop_vreg_use(b_raw);
        emit(c, mh);
        MInst ml = minst_make(MI_A64_MUL, 8u, loc);
        ml.op_count = 3u; ml.ops[0] = mop_vreg_def(dst_val); ml.ops[1] = mop_vreg_use(a_raw); ml.ops[2] = mop_vreg_use(b_raw);
        emit(c, ml);
        if (is_signed) {
            MInst cmp = minst_make(MI_A64_CMP_ASR63, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_vreg_use(dst_val);
            emit(c, cmp);
        } else {
            MInst cmp = minst_make(MI_A64_CMP_RI, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_imm(0);
            emit(c, cmp);
        }
        ovf_cond = MCOND_NE;
    }

    if (emit_cset) {
        MInst cs = minst_make(MI_A64_CSET, 4u, loc);
        cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst_ovf); cs.ops[1] = mop_cond(ovf_cond);
        emit(c, cs);
    }
    return ovf_cond;
}

static MCond emit_scalar_ovf_core(ISelCtx *c, PirOpcode op, PirType ty,
                                  const PirOperand *op_a, const PirOperand *op_b,
                                  MVReg dst_val, MVReg dst_ovf, uint32_t loc,
                                  bool emit_cset) {
    bool is_signed = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_SUB_OVF_S || op == PIR_OP_MUL_OVF_S);
    uint32_t bits  = pir_scalar_bits(ty.u.scalar, 8u);
    uint8_t w      = pir_type_vreg_bytes(ty);
    MCond ovf_cond = MCOND_NE;

    if (bits < 32u) {
        /* Sub-word (i8/i16): extend to 32-bit, perform op, compare against re-extended result. */
        MVReg a_raw = mat_op(c, op_a, loc);
        MVReg b_raw = mat_op(c, op_b, loc);
        MVReg a = normalize_int(c, a_raw, ty, is_signed, loc);
        MVReg b = normalize_int(c, b_raw, ty, is_signed, loc);
        MOpcode alu = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U) ? MI_A64_ADD_RR
                    : (op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) ? MI_A64_SUB_RR
                                                                         : MI_A64_MUL;
        MInst m = minst_make(alu, 4u, loc);
        m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a); m.ops[2] = mop_vreg_use(b);
        emit(c, m);
        MVReg trunc_back = normalize_int(c, dst_val, ty, is_signed, loc);
        MInst cmp = minst_make(MI_A64_CMP_RR, 4u, loc);
        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(dst_val); cmp.ops[1] = mop_vreg_use(trunc_back);
        emit(c, cmp);
        ovf_cond = MCOND_NE;
    } else if (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U) {
        int64_t imm = 0;
        if (op_is_uimm12(op_b, &imm)) {
            MVReg a_raw = mat_op(c, op_a, loc);
            MInst m = minst_make(MI_A64_ADDS_RI, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_imm(imm);
            emit(c, m);
        } else if (op_is_uimm12(op_a, &imm)) {
            MVReg b_raw = mat_op(c, op_b, loc);
            MInst m = minst_make(MI_A64_ADDS_RI, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(b_raw); m.ops[2] = mop_imm(imm);
            emit(c, m);
        } else {
            MVReg a_raw = mat_op(c, op_a, loc);
            MVReg b_raw = mat_op(c, op_b, loc);
            MInst m = minst_make(MI_A64_ADDS_RR, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_vreg_use(b_raw);
            emit(c, m);
        }
        ovf_cond = is_signed ? MCOND_VS : MCOND_CS;
    } else if (op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) {
        int64_t imm = 0;
        MVReg a_raw = mat_op(c, op_a, loc);
        if (op_is_uimm12(op_b, &imm)) {
            MInst m = minst_make(MI_A64_SUBS_RI, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_imm(imm);
            emit(c, m);
        } else {
            MVReg b_raw = mat_op(c, op_b, loc);
            MInst m = minst_make(MI_A64_SUBS_RR, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst_val); m.ops[1] = mop_vreg_use(a_raw); m.ops[2] = mop_vreg_use(b_raw);
            emit(c, m);
        }
        ovf_cond = is_signed ? MCOND_VS : MCOND_CC;
    } else if (bits == 32u) {
        MVReg a_raw = mat_op(c, op_a, loc);
        MVReg b_raw = mat_op(c, op_b, loc);
        MVReg wide = new_gp(c, 8u);
        MInst mw = minst_make(is_signed ? MI_A64_SMULL : MI_A64_UMULL, 8u, loc);
        mw.op_count = 3u; mw.ops[0] = mop_vreg_def(wide); mw.ops[1] = mop_vreg_use(a_raw); mw.ops[2] = mop_vreg_use(b_raw);
        emit(c, mw);
        MInst lo = minst_make(MI_A64_MOV_RR, 4u, loc);
        lo.op_count = 2u; lo.ops[0] = mop_vreg_def(dst_val); lo.ops[1] = mop_vreg_use(wide);
        emit(c, lo);
        if (is_signed) {
            MInst cmp = minst_make(MI_A64_CMP_SXTW, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(wide); cmp.ops[1] = mop_vreg_use(dst_val);
            emit(c, cmp);
        } else {
            MVReg hi = new_gp(c, 8u);
            MInst sh = minst_make(MI_A64_LSR_RI, 8u, loc);
            sh.op_count = 3u; sh.ops[0] = mop_vreg_def(hi); sh.ops[1] = mop_vreg_use(wide); sh.ops[2] = mop_imm(32);
            emit(c, sh);
            MInst cmp = minst_make(MI_A64_CMP_RI, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_imm(0);
            emit(c, cmp);
        }
        ovf_cond = MCOND_NE;
    } else {
        /* 64-bit mul overflow: mul + smulh/umulh */
        MVReg a_raw = mat_op(c, op_a, loc);
        MVReg b_raw = mat_op(c, op_b, loc);
        MVReg hi = new_gp(c, 8u);
        MInst mh = minst_make(is_signed ? MI_A64_SMULH : MI_A64_UMULH, 8u, loc);
        mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi); mh.ops[1] = mop_vreg_use(a_raw); mh.ops[2] = mop_vreg_use(b_raw);
        emit(c, mh);
        MInst ml = minst_make(MI_A64_MUL, 8u, loc);
        ml.op_count = 3u; ml.ops[0] = mop_vreg_def(dst_val); ml.ops[1] = mop_vreg_use(a_raw); ml.ops[2] = mop_vreg_use(b_raw);
        emit(c, ml);
        if (is_signed) {
            MInst cmp = minst_make(MI_A64_CMP_ASR63, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_vreg_use(dst_val);
            emit(c, cmp);
        } else {
            MInst cmp = minst_make(MI_A64_CMP_RI, 8u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(hi); cmp.ops[1] = mop_imm(0);
            emit(c, cmp);
        }
        ovf_cond = MCOND_NE;
    }

    if (emit_cset) {
        MInst cs = minst_make(MI_A64_CSET, 4u, loc);
        cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst_ovf); cs.ops[1] = mop_cond(ovf_cond);
        emit(c, cs);
    }
    return ovf_cond;
}

static void emit_scalar_ovf(ISelCtx *c, PirOpcode op, PirType ty,
                            const PirOperand *op_a, const PirOperand *op_b,
                            MVReg dst_val, MVReg dst_ovf, uint32_t loc) {
    (void)emit_scalar_ovf_core(c, op, ty, op_a, op_b, dst_val, dst_ovf, loc, true);
}

/* =========================================================================
 * Call Lowering (AAPCS64 Darwin & Linux + __divti3 Adapter)
 * ========================================================================= */

typedef struct {
    bool    is_reg;
    MPReg   preg;
    int64_t stack_off;
    uint8_t mem_bytes;
    bool    promote_f32_to_f64;
} ArgLoc;

static void plan_call_args(const PirTargetSpec *target,
                           const PirType *arg_tys, const uint32_t *arg_aligns,
                           uint32_t arg_count, bool is_variadic, uint32_t fixed_count,
                           ArgLoc *locs, uint64_t *out_stack_bytes,
                           uint32_t *out_gp_used, uint32_t *out_fp_used) {
    bool is_darwin = (target->os == PIR_OS_MACOS);
    uint32_t gp = 0u, fp = 0u;
    uint64_t st_off = 0u;

    for (uint32_t i = 0u; i < arg_count; ++i) {
        memset(&locs[i], 0, sizeof(locs[i]));
        PirType ty = arg_tys[i];
        bool is_va_arg = is_variadic && (i >= fixed_count);
        bool is_fp = (pir_type_rclass(ty) == MREG_CLASS_FP);
        uint8_t mb = pir_type_mem_bytes(ty);
        locs[i].mem_bytes = mb;

        if (is_darwin && is_va_arg) {
            /* Darwin ARM64: ALL variadic arguments pass strictly on the stack (min 8B; 16B if over-aligned). */
            uint64_t al = 8u;
            if (arg_aligns && arg_aligns[i] > al) al = arg_aligns[i];
            st_off = align_up_u64(st_off, al);
            locs[i].is_reg    = false;
            locs[i].stack_off = (int64_t)st_off;
            if (ty.kind == PIR_TYPE_SCALAR && ty.u.scalar == PIR_SCALAR_F32) {
                locs[i].promote_f32_to_f64 = true;
                locs[i].mem_bytes = 8u;
                st_off += 8u;
            } else {
                st_off += (mb > 8u) ? align_up_u64(mb, 8u) : 8u;
            }
            continue;
        }

        if (is_fp) {
            if (fp < 8u) {
                locs[i].is_reg = true;
                locs[i].preg   = A64_FP(fp++);
            } else {
                uint64_t al = is_darwin ? (uint64_t)mb : (mb > 8u ? (uint64_t)mb : 8u);
                if (arg_aligns && arg_aligns[i] > al) al = arg_aligns[i];
                uint64_t sz = is_darwin ? (uint64_t)mb : (mb > 8u ? (uint64_t)mb : 8u);
                st_off = align_up_u64(st_off, al);
                locs[i].is_reg    = false;
                locs[i].stack_off = (int64_t)st_off;
                st_off += sz;
            }
        } else {
            if (gp < 8u) {
                locs[i].is_reg = true;
                locs[i].preg   = A64_GP(gp++);
            } else {
                uint64_t al = is_darwin ? (uint64_t)mb : 8u;
                if (arg_aligns && arg_aligns[i] > al) al = arg_aligns[i];
                uint64_t sz = is_darwin ? (uint64_t)mb : 8u;
                st_off = align_up_u64(st_off, al);
                locs[i].is_reg    = false;
                locs[i].stack_off = (int64_t)st_off;
                st_off += sz;
            }
        }
    }

    *out_stack_bytes = st_off;
    if (out_gp_used) *out_gp_used = gp;
    if (out_fp_used) *out_fp_used = fp;
}

static bool is_ti3_libcall(const char *name) {
    return strcmp(name, "__divti3") == 0 || strcmp(name, "__udivti3") == 0 ||
           strcmp(name, "__modti3") == 0 || strcmp(name, "__umodti3") == 0;
}

static void emit_call_inst(ISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);
    uint32_t nargs = inst->operand_count - 1u;

    /* Direct ARM64 hardware fsqrt / fabs for standard C99 math libcalls (IEEE-754 exact). */
    if (!is_ind && inst->operands[0].kind == PIR_OPER_SYM &&
        nargs == 1u && inst->results[0] != PIR_INVALID_ID) {
        const char *cname = inst->operands[0].u.sym.name;
        bool is_defined_local = (inst->operands[0].u.sym.is_func &&
                                 inst->operands[0].u.sym.id < c->mod->funcs.len &&
                                 c->mod->funcs.data[inst->operands[0].u.sym.id].is_defined);
        if (!is_defined_local && cname) {
            PirType arg_ty = (inst->operands[1].kind == PIR_OPER_VAL)
                             ? c->fn->values.data[inst->operands[1].u.val].type
                             : inst->operands[1].type;
            MOpcode fp_op = MI_NOP;
            uint8_t fp_w  = 0u;
            if (inst->ty.kind == PIR_TYPE_SCALAR && arg_ty.kind == PIR_TYPE_SCALAR &&
                inst->ty.u.scalar == arg_ty.u.scalar) {
                if (inst->ty.u.scalar == PIR_SCALAR_F64) {
                    if (strcmp(cname, "sqrt") == 0)      { fp_op = MI_A64_FSQRT; fp_w = 8u; }
                    else if (strcmp(cname, "fabs") == 0) { fp_op = MI_A64_FABS;  fp_w = 8u; }
                } else if (inst->ty.u.scalar == PIR_SCALAR_F32) {
                    if (strcmp(cname, "sqrtf") == 0)      { fp_op = MI_A64_FSQRT; fp_w = 4u; }
                    else if (strcmp(cname, "fabsf") == 0) { fp_op = MI_A64_FABS;  fp_w = 4u; }
                }
            }
            if (fp_op != MI_NOP) {
                MVReg src = mat_op(c, &inst->operands[1], loc);
                MVReg dst = c->val_vreg[inst->results[0]];
                MInst fp = minst_make(fp_op, fp_w, loc);
                fp.op_count = 2u;
                fp.ops[0]   = mop_vreg_def(dst);
                fp.ops[1]   = mop_vreg_use(src);
                emit(c, fp);
                return;
            }
        }
    }

    c->mf->has_calls = true;

    /* Native compiler-rt adapter for 5-arg (__out, alo, ahi, blo, bhi) __divti3/__udivti3/__modti3/__umodti3 */
    if (!is_ind && inst->operands[0].kind == PIR_OPER_SYM &&
        is_ti3_libcall(inst->operands[0].u.sym.name) && nargs == 5u &&
        inst->results[0] == PIR_INVALID_ID) {
        MVReg out_ptr = mat_op(c, &inst->operands[1], loc);
        MVReg a0 = mat_op(c, &inst->operands[2], loc);
        MVReg a1 = mat_op(c, &inst->operands[3], loc);
        MVReg a2 = mat_op(c, &inst->operands[4], loc);
        MVReg a3 = mat_op(c, &inst->operands[5], loc);
        MVReg args4[4] = { a0, a1, a2, a3 };
        for (uint32_t k = 0u; k < 4u; ++k) {
            MInst mv = minst_make(MI_COPY, 8u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_preg_def(A64_GP(k)); mv.ops[1] = mop_vreg_use(args4[k]);
            emit(c, mv);
        }
        MInst bl = minst_make(MI_A64_BL, 8u, loc);
        bl.is_call  = true;
        bl.op_count = 1u;
        bl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, bl);

        /* Compiler-rt returns 128-bit result in (x0=lo, x1=hi); store into [out_ptr, #0/#8]. */
        MVReg rlo = new_gp(c, 8u);
        MVReg rhi = new_gp(c, 8u);
        MInst c0 = minst_make(MI_COPY, 8u, loc);
        c0.op_count = 2u; c0.ops[0] = mop_vreg_def(rlo); c0.ops[1] = mop_preg_use(A64_GP(0u));
        emit(c, c0);
        MInst c1 = minst_make(MI_COPY, 8u, loc);
        c1.op_count = 2u; c1.ops[0] = mop_vreg_def(rhi); c1.ops[1] = mop_preg_use(A64_GP(1u));
        emit(c, c1);

        MInst s0 = minst_make(MI_A64_STR, 8u, loc);
        s0.op_count = 3u; s0.ops[0] = mop_vreg_use(rlo); s0.ops[1] = mop_vreg_use(out_ptr); s0.ops[2] = mop_imm(0);
        emit(c, s0);
        MInst s1 = minst_make(MI_A64_STR, 8u, loc);
        s1.op_count = 3u; s1.ops[0] = mop_vreg_use(rhi); s1.ops[1] = mop_vreg_use(out_ptr); s1.ops[2] = mop_imm(8);
        emit(c, s1);
        return;
    }

    bool is_variadic = false;
    uint32_t fixed_count = nargs;
    PirType ret_ty = inst->ty;
    PirCabiRetInfo cabi_ret;
    memset(&cabi_ret, 0, sizeof(cabi_ret));
    const PirParamDecl *cal_params = NULL;
    uint32_t cal_param_count = 0u;
    if (is_ind) {
        const PirSigDef *sig = &c->mod->sigs.data[inst->sig_id];
        is_variadic     = sig->is_variadic;
        fixed_count     = sig->param_count;
        ret_ty          = sig->ret_type;
        cabi_ret        = sig->cabi_ret;
        cal_params      = sig->params;
        cal_param_count = sig->param_count;
    } else if (inst->operands[0].u.sym.is_func && inst->operands[0].u.sym.id < c->mod->funcs.len) {
        const PirFunction *cal = &c->mod->funcs.data[inst->operands[0].u.sym.id];
        is_variadic     = cal->is_variadic;
        fixed_count     = cal->param_count;
        ret_ty          = cal->ret_type;
        cabi_ret        = cal->cabi_ret;
        cal_params      = cal->params;
        cal_param_count = cal->param_count;
    }

    bool has_hidden_out = (cabi_ret.kind == PIR_CABI_RET_SRET_X8 ||
                           cabi_ret.kind == PIR_CABI_RET_MULTI_REG) && nargs > 0u;
    uint32_t arg_skip = has_hidden_out ? 1u : 0u;
    uint32_t pass_nargs = nargs - arg_skip;
    uint32_t pass_fixed = (fixed_count >= arg_skip) ? (fixed_count - arg_skip) : 0u;

    PirType  *arg_tys    = pass_nargs ? (PirType *)calloc(pass_nargs, sizeof(PirType)) : NULL;
    uint32_t *arg_aligns = pass_nargs ? (uint32_t *)calloc(pass_nargs, sizeof(uint32_t)) : NULL;
    ArgLoc   *alocs      = pass_nargs ? (ArgLoc *)calloc(pass_nargs, sizeof(ArgLoc)) : NULL;
    MVReg    *avregs     = pass_nargs ? (MVReg *)calloc(pass_nargs, sizeof(MVReg)) : NULL;
    if (pass_nargs && (!arg_tys || !arg_aligns || !alocs || !avregs)) abort();

    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        arg_tys[i] = inst->operands[i + 1u + arg_skip].type;
        if (cal_params && (i + arg_skip) < cal_param_count) {
            arg_aligns[i] = cal_params[i + arg_skip].abi_stack_align;
        }
        if (inst->operands[i + 1u + arg_skip].call_stack_align > arg_aligns[i]) {
            arg_aligns[i] = inst->operands[i + 1u + arg_skip].call_stack_align;
        }
    }
    uint64_t stack_bytes = 0u;
    plan_call_args(c->target, arg_tys, arg_aligns, pass_nargs, is_variadic, pass_fixed,
                   alocs, &stack_bytes, NULL, NULL);

    if (stack_bytes > c->mf->max_out_args_bytes) {
        c->mf->max_out_args_bytes = stack_bytes;
    }

    /* Materialize callee fn_ptr (if indirect), hidden out ptr, and all args BEFORE touching ABI regs. */
    MVReg fn_ptr_v = MVREG_NONE;
    if (is_ind) {
        fn_ptr_v = mat_op(c, &inst->operands[0], loc);
    }
    MVReg hidden_out_v = MVREG_NONE;
    if (has_hidden_out) {
        hidden_out_v = mat_op(c, &inst->operands[1], loc);
    }
    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        MVReg v = mat_op(c, &inst->operands[i + 1u + arg_skip], loc);
        if (alocs[i].promote_f32_to_f64) {
            MVReg d = new_fp(c, 8u);
            MInst cv = minst_make(MI_A64_FCVT, 8u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(d); cv.ops[1] = mop_vreg_use(v);
            emit(c, cv);
            v = d;
        } else if (alocs[i].is_reg && cal_params && (i + arg_skip) < cal_param_count &&
                   arg_tys[i].kind == PIR_TYPE_SCALAR &&
                   (arg_tys[i].u.scalar == PIR_SCALAR_I8 || arg_tys[i].u.scalar == PIR_SCALAR_I16)) {
            uint32_t pa = cal_params[i + arg_skip].attrs;
            if ((pa & (PIR_PATTR_SIGNEXT | PIR_PATTR_ZEROEXT)) != 0u) {
                v = normalize_int(c, v, arg_tys[i], (pa & PIR_PATTR_SIGNEXT) != 0u, loc);
            }
        } else if (is_variadic && i >= pass_fixed && c->target->os == PIR_OS_MACOS &&
                   arg_tys[i].kind == PIR_TYPE_SCALAR &&
                   (arg_tys[i].u.scalar == PIR_SCALAR_I8 || arg_tys[i].u.scalar == PIR_SCALAR_I16 ||
                    arg_tys[i].u.scalar == PIR_SCALAR_I32)) {
            /* Zero/sign-safe 8B slot write for Darwin variadic int args */
            alocs[i].mem_bytes = pir_type_mem_bytes(arg_tys[i]);
        }
        avregs[i] = v;
    }

    /* If dynamic alloca is present, adjust SP down right before storing outgoing stack args. */
    if (stack_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_DOWN, 8u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)stack_bytes);
        emit(c, adj);
    }

    /* 1. Store stack arguments first (so reg-arg physical regs don't get clobbered). */
    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        if (!alocs[i].is_reg) {
            MInst st = minst_make(MI_STORE_OUTARG, alocs[i].mem_bytes, loc);
            st.op_count = 2u;
            st.ops[0]   = mop_vreg_use(avregs[i]);
            st.ops[1]   = mop_imm(alocs[i].stack_off);
            emit(c, st);
        }
    }

    /* 2. Move register arguments into fixed x0..x7 / v0..v7. */
    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        if (alocs[i].is_reg) {
            uint8_t w = pir_type_vreg_bytes(arg_tys[i]);
            MInst mv = minst_make(MI_COPY, w, loc);
            mv.op_count = 2u;
            mv.ops[0]   = mop_preg_def(alocs[i].preg);
            mv.ops[1]   = mop_vreg_use(avregs[i]);
            emit(c, mv);
        }
    }

    /* 3. Emit BL or BLR (plus x8 sret copy if needed). For BLR, copy fn_ptr_v into x9. */
    if (is_ind) {
        MInst mv = minst_make(MI_COPY, 8u, loc);
        mv.op_count = 2u; mv.ops[0] = mop_preg_def(A64_GP(9u)); mv.ops[1] = mop_vreg_use(fn_ptr_v);
        emit(c, mv);
    }
    if (cabi_ret.kind == PIR_CABI_RET_SRET_X8 && hidden_out_v != MVREG_NONE) {
        MInst ms = minst_make(MI_COPY, 8u, loc);
        ms.op_count = 2u; ms.ops[0] = mop_preg_def(A64_GP(8u)); ms.ops[1] = mop_vreg_use(hidden_out_v);
        emit(c, ms);
    }
    if (is_ind) {
        MInst blr = minst_make(MI_A64_BLR, 8u, loc);
        blr.is_call  = true;
        blr.op_count = 1u;
        blr.ops[0]   = mop_preg_use(A64_GP(9u));
        emit(c, blr);
    } else {
        MInst bl = minst_make(MI_A64_BL, 8u, loc);
        bl.is_call  = true;
        bl.op_count = 1u;
        bl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, bl);
    }

    if (stack_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_UP, 8u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)stack_bytes);
        emit(c, adj);
    }

    /* 4. Capture return value(s). */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && hidden_out_v != MVREG_NONE) {
        MVReg rvs[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
        for (uint32_t k = 0u; k < cabi_ret.reg_count; ++k) {
            PirType rt = cabi_ret.regs[k];
            MPReg rreg = (pir_type_rclass(rt) == MREG_CLASS_FP) ? A64_FP(k) : A64_GP(k);
            uint8_t w  = pir_type_vreg_bytes(rt);
            rvs[k] = new_vreg_for_type(c, rt);
            MInst cp = minst_make(MI_COPY, w, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(rvs[k]); cp.ops[1] = mop_preg_use(rreg);
            emit(c, cp);
        }
        uint64_t rem = cabi_ret.agg_size;
        uint64_t off = 0u;
        for (uint32_t k = 0u; k < cabi_ret.reg_count && rem > 0u; ++k) {
            uint8_t mb = pir_type_mem_bytes(cabi_ret.regs[k]);
            if (rem >= mb) {
                MInst st = minst_make(MI_A64_STR, mb, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(rvs[k]);
                st.ops[1] = mop_vreg_use(hidden_out_v);
                st.ops[2] = mop_imm((int64_t)off);
                emit(c, st);
                off += mb;
                rem -= mb;
            } else {
                /* Partial trailing GP register (e.g. 9B/12B struct): store exact remaining bytes. */
                MVReg cur = rvs[k];
                uint64_t cur_shift = 0u;
                static const uint8_t chunks[3] = { 4u, 2u, 1u };
                for (uint32_t ci = 0u; ci < 3u && rem > 0u; ++ci) {
                    uint8_t cw = chunks[ci];
                    while (rem >= cw) {
                        MVReg sv = cur;
                        if (cur_shift > 0u) {
                            sv = new_gp(c, 8u);
                            MInst sh = minst_make(MI_A64_LSR_RI, 8u, loc);
                            sh.op_count = 3u;
                            sh.ops[0] = mop_vreg_def(sv);
                            sh.ops[1] = mop_vreg_use(cur);
                            sh.ops[2] = mop_imm((int64_t)cur_shift);
                            emit(c, sh);
                        }
                        MInst st = minst_make(MI_A64_STR, cw, loc);
                        st.op_count = 3u;
                        st.ops[0] = mop_vreg_use(sv);
                        st.ops[1] = mop_vreg_use(hidden_out_v);
                        st.ops[2] = mop_imm((int64_t)off);
                        emit(c, st);
                        off       += cw;
                        rem       -= cw;
                        cur_shift += (uint64_t)cw * 8u;
                    }
                }
            }
        }
    } else if (inst->results[0] != PIR_INVALID_ID &&
               !(ret_ty.kind == PIR_TYPE_SCALAR && ret_ty.u.scalar == PIR_SCALAR_VOID)) {
        MVReg dst = c->val_vreg[inst->results[0]];
        MPReg rreg = (pir_type_rclass(ret_ty) == MREG_CLASS_FP) ? A64_FP(0u) : A64_GP(0u);
        uint8_t w  = pir_type_vreg_bytes(ret_ty);
        MInst cp = minst_make(MI_COPY, w, loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(rreg);
        emit(c, cp);
    }

    free(arg_tys);
    free(arg_aligns);
    free(alocs);
    free(avregs);
}

/* =========================================================================
 * Addressing Mode Matcher (ptr_add + load/store fusion)
 * ========================================================================= */

static bool try_match_mem_addr(ISelCtx *c, const PirOperand *ptr_op, uint8_t width,
                               MVReg *out_base, int64_t *out_imm,
                               bool *out_has_idx, MVReg *out_idx, uint8_t *out_shift,
                               bool *out_sxtw, uint32_t loc) {
    *out_has_idx = false;
    *out_imm     = 0;
    *out_shift   = 0u;
    *out_sxtw    = false;

    if (ptr_op->kind == PIR_OPER_VAL) {
        PirValueId vid = ptr_op->u.val;
        const PirValueDef *vd = &c->fn->values.data[vid];
        if (vd->def_inst != PIR_INVALID_ID &&
            c->inst_fused[vd->def_inst] &&
            c->fn->insts.data[vd->def_inst].op == PIR_OP_PTR_ADD) {
            const PirInst *pa = &c->fn->insts.data[vd->def_inst];
            /* Constant offset fusion: [base, #imm] (supports multi-use across blocks when fused!) */
            if (pa->operands[0].kind == PIR_OPER_VAL && pa->operands[1].kind == PIR_OPER_IMM_INT) {
                int64_t off = (int64_t)eval_imm_u64(&pa->operands[1]);
                bool fits = (off >= -256 && off <= 255) ||
                            (off >= 0 && width > 0u && (off % width) == 0 && (off / width) <= 4095);
                if (fits) {
                    *out_base = c->val_vreg[pa->operands[0].u.val];
                    *out_imm  = off;
                    return true;
                }
            }
            /* ptr_add.i32 -> [base, wIdx, sxtw] */
            if (pa->operands[0].kind == PIR_OPER_VAL && pa->operands[1].kind == PIR_OPER_VAL &&
                pa->ty.kind == PIR_TYPE_SCALAR && pa->ty.u.scalar == PIR_SCALAR_I32 &&
                c->val_uses[vid] == 1u) {
                *out_base    = c->val_vreg[pa->operands[0].u.val];
                *out_has_idx = true;
                *out_idx     = c->val_vreg[pa->operands[1].u.val];
                *out_shift   = 0u;
                *out_sxtw    = true;
                return true;
            }
            /* Scaled/extended register index fusion: [base, idx, lsl/sxtw #scale] */
            if (pa->operands[0].kind == PIR_OPER_VAL && pa->operands[1].kind == PIR_OPER_VAL &&
                pa->ty.kind == PIR_TYPE_SCALAR && pa->ty.u.scalar == PIR_SCALAR_I64 &&
                c->val_uses[vid] == 1u) {
                PirValueId idx_v = pa->operands[1].u.val;
                uint8_t sh_amt = 0u;
                const PirValueDef *ivd = &c->fn->values.data[idx_v];
                if (ivd->def_inst != PIR_INVALID_ID &&
                    c->inst_fused[ivd->def_inst] &&
                    c->fn->insts.data[ivd->def_inst].op == PIR_OP_SHL &&
                    (c->fn->insts.data[ivd->def_inst].flags & PIR_FLAG_TRAP) == 0u &&
                    c->fn->insts.data[ivd->def_inst].operands[0].kind == PIR_OPER_VAL &&
                    c->fn->insts.data[ivd->def_inst].operands[1].kind == PIR_OPER_IMM_INT &&
                    c->val_uses[idx_v] == 1u) {
                    uint64_t sh = eval_imm_u64(&c->fn->insts.data[ivd->def_inst].operands[1]);
                    if ((sh == 0u) || (sh < 64u && width > 1u && ((uint64_t)1u << sh) == (uint64_t)width)) {
                        sh_amt = (uint8_t)sh;
                        idx_v  = c->fn->insts.data[ivd->def_inst].operands[0].u.val;
                    }
                }
                /* Check if idx_v is a fused sext.i64(i32) -> [base, wIdx, sxtw #sh_amt] */
                const PirValueDef *svd = &c->fn->values.data[idx_v];
                if (svd->def_inst != PIR_INVALID_ID &&
                    c->inst_fused[svd->def_inst] &&
                    c->fn->insts.data[svd->def_inst].op == PIR_OP_SEXT &&
                    c->fn->insts.data[svd->def_inst].operands[0].kind == PIR_OPER_VAL) {
                    PirValueId raw32 = c->fn->insts.data[svd->def_inst].operands[0].u.val;
                    PirType raw_ty = c->fn->values.data[raw32].type;
                    if (raw_ty.kind == PIR_TYPE_SCALAR && raw_ty.u.scalar == PIR_SCALAR_I32) {
                        *out_base    = c->val_vreg[pa->operands[0].u.val];
                        *out_has_idx = true;
                        *out_idx     = c->val_vreg[raw32];
                        *out_shift   = sh_amt;
                        *out_sxtw    = true;
                        return true;
                    }
                }
                *out_base    = c->val_vreg[pa->operands[0].u.val];
                *out_has_idx = true;
                *out_idx     = c->val_vreg[idx_v];
                *out_shift   = sh_amt;
                *out_sxtw    = false;
                return true;
            }
        }
    }
    *out_base = mat_op(c, ptr_op, loc);
    return false;
}

/* =========================================================================
 * Instruction Selection for Every LIR Opcode (Exhaustive Switch)
 * ========================================================================= */

static bool isel_inst(ISelCtx *c, PirInstId iid, PirBlockId cur_pir_blk) {
    if (c->inst_fused[iid]) return true;
    const PirInst *inst = &c->fn->insts.data[iid];
    if (inst->op != PIR_OP_SELECT) {
        c->last_flags_cmp_id = PIR_INVALID_ID;
    }
    uint32_t loc = inst->loc_idx;
    MVReg dst0 = (inst->results[0] != PIR_INVALID_ID) ? c->val_vreg[inst->results[0]] : MVREG_NONE;
    MVReg dst1 = (inst->results[1] != PIR_INVALID_ID) ? c->val_vreg[inst->results[1]] : MVREG_NONE;

    switch (inst->op) {
        case PIR_OP_CONST: {
            const PirOperand *op = &inst->operands[0];
            if (op->kind == PIR_OPER_SYM) {
                emit_sym_addr(c, dst0, op->u.sym.name, op->u.sym.offset, loc);
            } else if (op->kind == PIR_OPER_IMM_INT) {
                emit_imm_gp(c, dst0, eval_imm_u64(op), pir_type_vreg_bytes(inst->ty), loc);
            } else if (op->kind == PIR_OPER_IMM_FLOAT) {
                uint8_t w = pir_type_vreg_bytes(inst->ty);
                MVReg gp = new_gp(c, w);
                emit_imm_gp(c, gp, op->u.imm_float.bits, w, loc);
                MInst fm = minst_make(MI_A64_FMOV_G2F, w, loc);
                fm.op_count = 2u; fm.ops[0] = mop_vreg_def(dst0); fm.ops[1] = mop_vreg_use(gp);
                emit(c, fm);
            } else {
                MVReg s = mat_op(c, op, loc);
                MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(inst->ty), loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(s);
                emit(c, cp);
            }
            return true;
        }

        case PIR_OP_ADDR:
            emit_sym_addr(c, dst0, inst->operands[0].u.sym.name, inst->operands[0].u.sym.offset, loc);
            return true;

        case PIR_OP_ADD:
        case PIR_OP_SUB:
        case PIR_OP_MUL: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType vt = inst->ty.u.vec;
                MVecArr arr = pir_vec_to_arr(vt);
                uint8_t vb  = (uint8_t)(pir_vec_bits(vt) / 8u);
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);
                if ((inst->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) {
                    bool is_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
                    PirOpcode ovf_op = (inst->op == PIR_OP_ADD) ? (is_s ? PIR_OP_ADD_OVF_S : PIR_OP_ADD_OVF_U)
                                     : (inst->op == PIR_OP_SUB) ? (is_s ? PIR_OP_SUB_OVF_S : PIR_OP_SUB_OVF_U)
                                                                : (is_s ? PIR_OP_MUL_OVF_S : PIR_OP_MUL_OVF_U);
                    PirScalarType el = pir_vec_elem_scalar(vt);
                    PirType el_ty = pir_type_scalar(el);
                    uint32_t lanes = pir_vec_lanes(vt);
                    uint8_t ew = pir_type_vreg_bytes(el_ty);
                    MVReg acc = a;
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        MVReg la = new_gp(c, ew), lb = new_gp(c, ew), lr = new_gp(c, ew);
                        emit_vec_lane_extract(c, la, a, vt, l, loc);
                        emit_vec_lane_extract(c, lb, b, vt, l, loc);
                        MCond ov_cond = emit_scalar_ovf_regs(c, ovf_op, el_ty, la, lb, lr, 0u, loc, false);
                        emit_cond_trap(c, ov_cond, loc);
                        MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, vb);
                        if (l == 0u) emit_vec_splat_scalar(c, nxt, lr, vt, loc);
                        else         emit_vec_lane_insert(c, nxt, acc, lr, vt, l, loc);
                        acc = nxt;
                    }
                    return true;
                }
                if (inst->op == PIR_OP_MUL && vt == PIR_VEC_V2I64) {
                    /* NEON has no mul.2d: lane-wise 64-bit mul */
                    MVReg acc = new_fp(c, vb);
                    for (uint32_t l = 0u; l < 2u; ++l) {
                        MVReg la = new_gp(c, 8u), lb = new_gp(c, 8u), lp = new_gp(c, 8u);
                        emit_vec_lane_extract(c, la, a, vt, l, loc);
                        emit_vec_lane_extract(c, lb, b, vt, l, loc);
                        MInst m = minst_make(MI_A64_MUL, 8u, loc);
                        m.op_count = 3u; m.ops[0] = mop_vreg_def(lp); m.ops[1] = mop_vreg_use(la); m.ops[2] = mop_vreg_use(lb);
                        emit(c, m);
                        MVReg nxt = (l == 1u) ? dst0 : new_fp(c, vb);
                        emit_vec_lane_insert(c, nxt, l == 0u ? a : acc, lp, vt, l, loc);
                        acc = nxt;
                    }
                    return true;
                }
                MOpcode mop = (inst->op == PIR_OP_ADD) ? MI_A64_VADD
                            : (inst->op == PIR_OP_SUB) ? MI_A64_VSUB : MI_A64_VMUL;
                MInst v = minst_make(mop, vb, loc);
                v.arr = arr; v.op_count = 3u;
                v.ops[0] = mop_vreg_def(dst0); v.ops[1] = mop_vreg_use(a); v.ops[2] = mop_vreg_use(b);
                emit(c, v);
                return true;
            }
            /* Scalar add/sub/mul */
            if ((inst->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) {
                bool is_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
                PirOpcode ovf_op = (inst->op == PIR_OP_ADD) ? (is_s ? PIR_OP_ADD_OVF_S : PIR_OP_ADD_OVF_U)
                                 : (inst->op == PIR_OP_SUB) ? (is_s ? PIR_OP_SUB_OVF_S : PIR_OP_SUB_OVF_U)
                                                            : (is_s ? PIR_OP_MUL_OVF_S : PIR_OP_MUL_OVF_U);
                MCond ov_cond = emit_scalar_ovf_core(c, ovf_op, inst->ty,
                                                     &inst->operands[0], &inst->operands[1],
                                                     dst0, 0u, loc, false);
                emit_cond_trap(c, ov_cond, loc);
                return true;
            }
            MVReg a = mat_op(c, &inst->operands[0], loc);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            if (inst->op == PIR_OP_ADD) {
                PirInstId shr_id = PIR_INVALID_ID, shl_id = PIR_INVALID_ID;
                PirOperand rx, rn;
                bool r_is_imm = false;
                uint32_t r_imm = 0u;
                if (try_match_ror(c->fn, c->val_uses, cur_pir_blk, inst,
                                  &shr_id, &shl_id, &rx, &r_is_imm, &r_imm, &rn) &&
                    c->inst_fused[shr_id] && c->inst_fused[shl_id]) {
                    MVReg vx = mat_op(c, &rx, loc);
                    MInst ror = minst_make(r_is_imm ? MI_A64_ROR_RI : MI_A64_ROR_RR, w, loc);
                    ror.op_count = 3u;
                    ror.ops[0] = mop_vreg_def(dst0);
                    ror.ops[1] = mop_vreg_use(vx);
                    ror.ops[2] = r_is_imm ? mop_imm((int64_t)r_imm) : mop_vreg_use(mat_op(c, &rn, loc));
                    emit(c, ror);
                    return true;
                }
                PirInstId mul_id = PIR_INVALID_ID;
                PirOperand m0, m1, ra;
                if (try_match_madd(c->fn, c->val_uses, cur_pir_blk, inst, &mul_id, &m0, &m1, &ra) &&
                    c->inst_fused[mul_id]) {
                    MVReg rm0 = mat_op(c, &m0, loc);
                    MVReg rm1 = mat_op(c, &m1, loc);
                    MVReg rra = mat_op(c, &ra, loc);
                    MInst ma = minst_make(MI_A64_MADD, w, loc);
                    ma.op_count = 4u;
                    ma.ops[0] = mop_vreg_def(dst0);
                    ma.ops[1] = mop_vreg_use(rm0);
                    ma.ops[2] = mop_vreg_use(rm1);
                    ma.ops[3] = mop_vreg_use(rra);
                    emit(c, ma);
                    return true;
                }
            }
            if ((inst->op == PIR_OP_ADD || inst->op == PIR_OP_SUB) &&
                inst->operands[1].kind == PIR_OPER_IMM_INT &&
                !inst->operands[1].u.imm_int.is_negative &&
                inst->operands[1].u.imm_int.lo <= 4095u) {
                MInst m = minst_make(inst->op == PIR_OP_ADD ? MI_A64_ADD_RI : MI_A64_SUB_RI, w, loc);
                m.op_count = 3u; m.ops[0] = mop_vreg_def(dst0); m.ops[1] = mop_vreg_use(a);
                m.ops[2] = mop_imm((int64_t)inst->operands[1].u.imm_int.lo);
                emit(c, m);
                return true;
            }
            MVReg b = mat_op(c, &inst->operands[1], loc);
            MOpcode mop = (inst->op == PIR_OP_ADD) ? MI_A64_ADD_RR
                        : (inst->op == PIR_OP_SUB) ? MI_A64_SUB_RR : MI_A64_MUL;
            MInst m = minst_make(mop, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst0); m.ops[1] = mop_vreg_use(a); m.ops[2] = mop_vreg_use(b);
            emit(c, m);
            return true;
        }

        case PIR_OP_DIV_S: case PIR_OP_DIV_U:
        case PIR_OP_REM_S: case PIR_OP_REM_U: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                /* Lane-wise vector integer div/rem */
                PirVecType vt = inst->ty.u.vec;
                PirScalarType el = pir_vec_elem_scalar(vt);
                PirType el_ty = pir_type_scalar(el);
                uint32_t lanes = pir_vec_lanes(vt);
                uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
                uint8_t ew = pir_type_vreg_bytes(el_ty);
                MVReg va = mat_op(c, &inst->operands[0], loc);
                MVReg vb_r = mat_op(c, &inst->operands[1], loc);
                MVReg acc = va;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg la = new_gp(c, ew), lb = new_gp(c, ew), lr = new_gp(c, ew);
                    emit_vec_lane_extract(c, la, va, vt, l, loc);
                    emit_vec_lane_extract(c, lb, vb_r, vt, l, loc);
                    emit_scalar_div_rem_regs(c, inst->op, el_ty, inst->flags, la, lb, lr, loc);
                    MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, vb);
                    if (l == 0u) emit_vec_splat_scalar(c, nxt, lr, vt, loc);
                    else         emit_vec_lane_insert(c, nxt, acc, lr, vt, l, loc);
                    acc = nxt;
                }
                return true;
            }
            emit_scalar_div_rem(c, inst, dst0, loc);
            return true;
        }

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U: {
            emit_scalar_ovf(c, inst->op, inst->ty, &inst->operands[0], &inst->operands[1], dst0, dst1, loc);
            return true;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            bool is_s = (inst->op == PIR_OP_MUL_WIDE_S);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (inst->ty.u.scalar == PIR_SCALAR_I32) {
                MVReg wide = new_gp(c, 8u);
                MInst mw = minst_make(is_s ? MI_A64_SMULL : MI_A64_UMULL, 8u, loc);
                mw.op_count = 3u; mw.ops[0] = mop_vreg_def(wide); mw.ops[1] = mop_vreg_use(a); mw.ops[2] = mop_vreg_use(b);
                emit(c, mw);
                MInst lo = minst_make(MI_A64_MOV_RR, 4u, loc);
                lo.op_count = 2u; lo.ops[0] = mop_vreg_def(dst0); lo.ops[1] = mop_vreg_use(wide);
                emit(c, lo);
                MVReg hi64 = new_gp(c, 8u);
                MInst sh = minst_make(MI_A64_LSR_RI, 8u, loc);
                sh.op_count = 3u; sh.ops[0] = mop_vreg_def(hi64); sh.ops[1] = mop_vreg_use(wide); sh.ops[2] = mop_imm(32);
                emit(c, sh);
                MInst hi = minst_make(MI_A64_MOV_RR, 4u, loc);
                hi.op_count = 2u; hi.ops[0] = mop_vreg_def(dst1); hi.ops[1] = mop_vreg_use(hi64);
                emit(c, hi);
            } else {
                /* 64x64 -> 128: compute hi into temp, then lo, then copy temp -> dst1 (alias-safe!) */
                MVReg hi_t = new_gp(c, 8u);
                MInst mh = minst_make(is_s ? MI_A64_SMULH : MI_A64_UMULH, 8u, loc);
                mh.op_count = 3u; mh.ops[0] = mop_vreg_def(hi_t); mh.ops[1] = mop_vreg_use(a); mh.ops[2] = mop_vreg_use(b);
                emit(c, mh);
                MInst ml = minst_make(MI_A64_MUL, 8u, loc);
                ml.op_count = 3u; ml.ops[0] = mop_vreg_def(dst0); ml.ops[1] = mop_vreg_use(a); ml.ops[2] = mop_vreg_use(b);
                emit(c, ml);
                MInst cp = minst_make(MI_COPY, 8u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst1); cp.ops[1] = mop_vreg_use(hi_t);
                emit(c, cp);
            }
            return true;
        }

        case PIR_OP_ADD_CARRY:
        case PIR_OP_SUB_BORROW: {
            bool is_add = (inst->op == PIR_OP_ADD_CARRY);
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            MVReg cin_raw = mat_op(c, &inst->operands[2], loc);
            MVReg cin = new_gp(c, 4u);
            MInst cmk = minst_make(MI_A64_AND_RI, 4u, loc);
            cmk.op_count = 3u; cmk.ops[0] = mop_vreg_def(cin); cmk.ops[1] = mop_vreg_use(cin_raw); cmk.ops[2] = mop_imm(1);
            emit(c, cmk);
            if (bits < 32u) {
                MVReg na = normalize_int(c, a, inst->ty, false, loc);
                MVReg nb = normalize_int(c, b, inst->ty, false, loc);
                MVReg t0 = new_gp(c, 4u), t1 = new_gp(c, 4u);
                if (is_add) {
                    MInst m1 = minst_make(MI_A64_ADD_RR, 4u, loc);
                    m1.op_count = 3u; m1.ops[0] = mop_vreg_def(t0); m1.ops[1] = mop_vreg_use(na); m1.ops[2] = mop_vreg_use(nb);
                    emit(c, m1);
                    MInst m2 = minst_make(MI_A64_ADD_RR, 4u, loc);
                    m2.op_count = 3u; m2.ops[0] = mop_vreg_def(t1); m2.ops[1] = mop_vreg_use(t0); m2.ops[2] = mop_vreg_use(cin);
                    emit(c, m2);
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(t1);
                    emit(c, cp);
                    MInst sh = minst_make(MI_A64_LSR_RI, 4u, loc);
                    sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst1); sh.ops[1] = mop_vreg_use(t1); sh.ops[2] = mop_imm((int64_t)bits);
                    emit(c, sh);
                } else {
                    MInst m1 = minst_make(MI_A64_SUB_RR, 4u, loc);
                    m1.op_count = 3u; m1.ops[0] = mop_vreg_def(t0); m1.ops[1] = mop_vreg_use(na); m1.ops[2] = mop_vreg_use(nb);
                    emit(c, m1);
                    MInst m2 = minst_make(MI_A64_SUB_RR, 4u, loc);
                    m2.op_count = 3u; m2.ops[0] = mop_vreg_def(t1); m2.ops[1] = mop_vreg_use(t0); m2.ops[2] = mop_vreg_use(cin);
                    emit(c, m2);
                    MInst cp = minst_make(MI_COPY, 4u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(t1);
                    emit(c, cp);
                    MInst sh = minst_make(MI_A64_LSR_RI, 4u, loc);
                    sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst1); sh.ops[1] = mop_vreg_use(t1); sh.ops[2] = mop_imm(31);
                    emit(c, sh);
                }
                return true;
            }
            if (is_add) {
                /* subs wzr, cin, #1 sets C=1 iff cin >= 1; adcs dst, a, b; cset cout, cs */
                MInst sc = minst_make(MI_A64_CMP_RI, 4u, loc);
                sc.op_count = 2u; sc.ops[0] = mop_vreg_use(cin); sc.ops[1] = mop_imm(1);
                emit(c, sc);
                MInst ac = minst_make(MI_A64_ADCS, w, loc);
                ac.op_count = 3u; ac.ops[0] = mop_vreg_def(dst0); ac.ops[1] = mop_vreg_use(a); ac.ops[2] = mop_vreg_use(b);
                emit(c, ac);
                MInst cs = minst_make(MI_A64_CSET, 4u, loc);
                cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst1); cs.ops[1] = mop_cond(MCOND_CS);
                emit(c, cs);
            } else {
                /* cmp wzr, cin sets C=1 iff bin==0, C=0 iff bin==1; sbcs dst, a, b; cset bout, cc */
                MVReg z = new_gp(c, 4u);
                emit_imm_gp(c, z, 0u, 4u, loc);
                MInst sc = minst_make(MI_A64_CMP_RR, 4u, loc);
                sc.op_count = 2u; sc.ops[0] = mop_vreg_use(z); sc.ops[1] = mop_vreg_use(cin);
                emit(c, sc);
                MInst sb = minst_make(MI_A64_SBCS, w, loc);
                sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst0); sb.ops[1] = mop_vreg_use(a); sb.ops[2] = mop_vreg_use(b);
                emit(c, sb);
                MInst cs = minst_make(MI_A64_CSET, 4u, loc);
                cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst1); cs.ops[1] = mop_cond(MCOND_CC);
                emit(c, cs);
            }
            return true;
        }

        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                uint8_t vb = (uint8_t)(pir_vec_bits(inst->ty.u.vec) / 8u);
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);
                MOpcode mop = (inst->op == PIR_OP_AND) ? MI_A64_VAND
                            : (inst->op == PIR_OP_OR)  ? MI_A64_VORR : MI_A64_VEOR;
                MInst v = minst_make(mop, vb, loc);
                v.arr = (vb == 8u) ? MARR_8B : MARR_16B;
                v.op_count = 3u; v.ops[0] = mop_vreg_def(dst0); v.ops[1] = mop_vreg_use(a); v.ops[2] = mop_vreg_use(b);
                emit(c, v);
                return true;
            }
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            if (inst->op == PIR_OP_OR || inst->op == PIR_OP_XOR) {
                PirInstId shr_id = PIR_INVALID_ID, shl_id = PIR_INVALID_ID;
                PirOperand rx, rn;
                bool r_is_imm = false;
                uint32_t r_imm = 0u;
                if (try_match_ror(c->fn, c->val_uses, cur_pir_blk, inst,
                                  &shr_id, &shl_id, &rx, &r_is_imm, &r_imm, &rn) &&
                    c->inst_fused[shr_id] && c->inst_fused[shl_id]) {
                    MVReg vx = mat_op(c, &rx, loc);
                    MInst ror = minst_make(r_is_imm ? MI_A64_ROR_RI : MI_A64_ROR_RR, w, loc);
                    ror.op_count = 3u;
                    ror.ops[0] = mop_vreg_def(dst0);
                    ror.ops[1] = mop_vreg_use(vx);
                    ror.ops[2] = r_is_imm ? mop_imm((int64_t)r_imm) : mop_vreg_use(mat_op(c, &rn, loc));
                    emit(c, ror);
                    return true;
                }
            }
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint64_t imm_v = eval_imm_u64(&inst->operands[1]);
                uint32_t bits = (w <= 4u) ? 32u : 64u;
                uint64_t full = (bits == 64u) ? UINT64_MAX : ((1ULL << bits) - 1ULL);
                if (is_arm64_bitmask_imm(imm_v, w)) {
                    MOpcode mop = (inst->op == PIR_OP_AND) ? MI_A64_AND_RI
                                : (inst->op == PIR_OP_OR)  ? MI_A64_ORR_RI : MI_A64_EOR_RI;
                    MInst m = minst_make(mop, w, loc);
                    m.op_count = 3u;
                    m.ops[0] = mop_vreg_def(dst0);
                    m.ops[1] = mop_vreg_use(a);
                    m.ops[2] = mop_imm((int64_t)(imm_v & full));
                    emit(c, m);
                    return true;
                }
            }
            MVReg b = mat_op(c, &inst->operands[1], loc);
            MOpcode mop = (inst->op == PIR_OP_AND) ? MI_A64_AND_RR
                        : (inst->op == PIR_OP_OR)  ? MI_A64_ORR_RR : MI_A64_EOR_RR;
            MInst m = minst_make(mop, w, loc);
            m.op_count = 3u; m.ops[0] = mop_vreg_def(dst0); m.ops[1] = mop_vreg_use(a); m.ops[2] = mop_vreg_use(b);
            emit(c, m);
            return true;
        }

        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType vt = inst->ty.u.vec;
                MVecArr arr = pir_vec_to_arr(vt);
                uint8_t vb  = (uint8_t)(pir_vec_bits(vt) / 8u);
                uint32_t ebits = pir_scalar_bits(pir_vec_elem_scalar(vt), 8u);
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);
                if ((inst->flags & PIR_FLAG_TRAP) != 0u) {
                    PirType el_ty = pir_type_scalar(pir_vec_elem_scalar(vt));
                    uint32_t lanes = pir_vec_lanes(vt);
                    uint8_t ew = pir_type_vreg_bytes(el_ty);
                    for (uint32_t l = 0u; l < lanes; ++l) {
                        MVReg lb = new_gp(c, ew);
                        emit_vec_lane_extract(c, lb, b, vt, l, loc);
                        lb = normalize_int(c, lb, el_ty, false, loc);
                        MInst cmp = minst_make(MI_A64_CMP_RI, ew, loc);
                        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(lb); cmp.ops[1] = mop_imm((int64_t)ebits);
                        emit(c, cmp);
                        emit_cond_trap(c, MCOND_CS, loc);
                    }
                }
                /* Mask shift amounts mod ebits */
                MVReg msk_s = new_gp(c, ebits == 64u ? 8u : 4u);
                emit_imm_gp(c, msk_s, ebits - 1u, ebits == 64u ? 8u : 4u, loc);
                MVReg msk_v = new_fp(c, vb);
                MInst dp = minst_make(MI_A64_VDUP_GP, vb, loc);
                dp.arr = arr; dp.op_count = 2u; dp.ops[0] = mop_vreg_def(msk_v); dp.ops[1] = mop_vreg_use(msk_s);
                emit(c, dp);
                MVReg sh_v = new_fp(c, vb);
                MInst an = minst_make(MI_A64_VAND, vb, loc);
                an.arr = (vb == 8u) ? MARR_8B : MARR_16B;
                an.op_count = 3u; an.ops[0] = mop_vreg_def(sh_v); an.ops[1] = mop_vreg_use(b); an.ops[2] = mop_vreg_use(msk_v);
                emit(c, an);
                if (inst->op == PIR_OP_SHR || inst->op == PIR_OP_SAR) {
                    /* NEON right shift uses negative shift count in sshl/ushl */
                    MVReg neg_v = new_fp(c, vb);
                    MInst ng = minst_make(MI_A64_VNEG, vb, loc);
                    ng.arr = arr; ng.op_count = 2u; ng.ops[0] = mop_vreg_def(neg_v); ng.ops[1] = mop_vreg_use(sh_v);
                    emit(c, ng);
                    sh_v = neg_v;
                }
                MInst sh = minst_make(inst->op == PIR_OP_SAR ? MI_A64_VSHL_S : MI_A64_VSHL_U, vb, loc);
                sh.arr = arr; sh.op_count = 3u;
                sh.ops[0] = mop_vreg_def(dst0); sh.ops[1] = mop_vreg_use(a); sh.ops[2] = mop_vreg_use(sh_v);
                emit(c, sh);
                return true;
            }
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w     = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->op == PIR_OP_SHR) a = normalize_int(c, a, inst->ty, false, loc);
            else if (inst->op == PIR_OP_SAR) a = normalize_int(c, a, inst->ty, true, loc);

            if ((inst->flags & PIR_FLAG_TRAP) != 0u) {
                MVReg b = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
                MInst cmp = minst_make(MI_A64_CMP_RI, w, loc);
                cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(b); cmp.ops[1] = mop_imm((int64_t)bits);
                emit(c, cmp);
                emit_cond_trap(c, MCOND_CS, loc);
                MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_A64_LSL_RR
                            : (inst->op == PIR_OP_SHR) ? MI_A64_LSR_RR : MI_A64_ASR_RR;
                MInst sh = minst_make(mop, w, loc);
                sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst0); sh.ops[1] = mop_vreg_use(a); sh.ops[2] = mop_vreg_use(b);
                emit(c, sh);
                return true;
            }

            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint64_t amt = eval_imm_u64(&inst->operands[1]) & (uint64_t)(bits - 1u);
                if (amt == 0u) {
                    MInst cp = minst_make(MI_COPY, w, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(a);
                    emit(c, cp);
                } else {
                    MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_A64_LSL_RI
                                : (inst->op == PIR_OP_SHR) ? MI_A64_LSR_RI : MI_A64_ASR_RI;
                    MInst sh = minst_make(mop, w, loc);
                    sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst0); sh.ops[1] = mop_vreg_use(a);
                    sh.ops[2] = mop_imm((int64_t)amt);
                    emit(c, sh);
                }
                return true;
            }
            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (bits < 32u) {
                MVReg msk = new_gp(c, 4u);
                emit_imm_gp(c, msk, bits - 1u, 4u, loc);
                MVReg bm = new_gp(c, 4u);
                MInst an = minst_make(MI_A64_AND_RR, 4u, loc);
                an.op_count = 3u; an.ops[0] = mop_vreg_def(bm); an.ops[1] = mop_vreg_use(b); an.ops[2] = mop_vreg_use(msk);
                emit(c, an);
                b = bm;
            }
            MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_A64_LSL_RR
                        : (inst->op == PIR_OP_SHR) ? MI_A64_LSR_RR : MI_A64_ASR_RR;
            MInst sh = minst_make(mop, w, loc);
            sh.op_count = 3u; sh.ops[0] = mop_vreg_def(dst0); sh.ops[1] = mop_vreg_use(a); sh.ops[2] = mop_vreg_use(b);
            emit(c, sh);
            return true;
        }

        case PIR_OP_CLZ: {
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, false, loc);
            if (bits < 32u) {
                MVReg t = new_gp(c, 4u);
                MInst cl = minst_make(MI_A64_CLZ, 4u, loc);
                cl.op_count = 2u; cl.ops[0] = mop_vreg_def(t); cl.ops[1] = mop_vreg_use(a);
                emit(c, cl);
                MInst sb = minst_make(MI_A64_SUB_RI, 4u, loc);
                sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst0); sb.ops[1] = mop_vreg_use(t);
                sb.ops[2] = mop_imm((int64_t)(32u - bits));
                emit(c, sb);
            } else {
                MInst cl = minst_make(MI_A64_CLZ, w, loc);
                cl.op_count = 2u; cl.ops[0] = mop_vreg_def(dst0); cl.ops[1] = mop_vreg_use(a);
                emit(c, cl);
            }
            return true;
        }

        case PIR_OP_CTZ: {
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (bits < 32u) {
                /* Set sentinel bit at position `bits` so rbit+clz returns `bits` when low bits are 0. */
                MVReg sbit = new_gp(c, 4u);
                emit_imm_gp(c, sbit, (uint64_t)1u << bits, 4u, loc);
                MVReg ao = new_gp(c, 4u);
                MInst o = minst_make(MI_A64_ORR_RR, 4u, loc);
                o.op_count = 3u; o.ops[0] = mop_vreg_def(ao); o.ops[1] = mop_vreg_use(a); o.ops[2] = mop_vreg_use(sbit);
                emit(c, o);
                a = ao;
            }
            MVReg rb = new_gp(c, w);
            MInst r = minst_make(MI_A64_RBIT, w, loc);
            r.op_count = 2u; r.ops[0] = mop_vreg_def(rb); r.ops[1] = mop_vreg_use(a);
            emit(c, r);
            MInst cl = minst_make(MI_A64_CLZ, w, loc);
            cl.op_count = 2u; cl.ops[0] = mop_vreg_def(dst0); cl.ops[1] = mop_vreg_use(rb);
            emit(c, cl);
            return true;
        }

        case PIR_OP_POPCNT: {
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, false, loc);
            MVReg a64 = a;
            if (w == 4u) {
                /* Zero-extend w -> x via mov w, w (writes 32-bit clearing upper 32 bits!). */
                a64 = new_gp(c, 8u);
                MInst zx = minst_make(MI_A64_MOV_RR, 4u, loc);
                zx.op_count = 2u; zx.ops[0] = mop_vreg_def(a64); zx.ops[1] = mop_vreg_use(a);
                emit(c, zx);
            }
            MVReg f0 = new_fp(c, 8u), f1 = new_fp(c, 8u), f2 = new_fp(c, 8u);
            MInst mv = minst_make(MI_A64_FMOV_G2F, 8u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(f0); mv.ops[1] = mop_vreg_use(a64);
            emit(c, mv);
            MInst cnt = minst_make(MI_A64_VCNT, 8u, loc);
            cnt.op_count = 2u; cnt.ops[0] = mop_vreg_def(f1); cnt.ops[1] = mop_vreg_use(f0);
            emit(c, cnt);
            MInst av = minst_make(MI_A64_VADDV, 8u, loc);
            av.op_count = 2u; av.ops[0] = mop_vreg_def(f2); av.ops[1] = mop_vreg_use(f1);
            emit(c, av);
            MInst ex = minst_make(MI_A64_VUMOV, 4u, loc);
            ex.arr = MARR_8B; ex.aux_width = 0u;
            ex.op_count = 2u; ex.ops[0] = mop_vreg_def(dst0); ex.ops[1] = mop_vreg_use(f2);
            emit(c, ex);
            return true;
        }

        case PIR_OP_BSWAP: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.u.scalar == PIR_SCALAR_I16) {
                MVReg t = new_gp(c, 4u);
                MInst rv = minst_make(MI_A64_REV16, 4u, loc);
                rv.op_count = 2u; rv.ops[0] = mop_vreg_def(t); rv.ops[1] = mop_vreg_use(a);
                emit(c, rv);
                MInst ux = minst_make(MI_A64_UXTH, 4u, loc);
                ux.op_count = 2u; ux.ops[0] = mop_vreg_def(dst0); ux.ops[1] = mop_vreg_use(t);
                emit(c, ux);
            } else {
                uint8_t w = pir_type_vreg_bytes(inst->ty);
                MInst rv = minst_make(MI_A64_REV, w, loc);
                rv.op_count = 2u; rv.ops[0] = mop_vreg_def(dst0); rv.ops[1] = mop_vreg_use(a);
                emit(c, rv);
            }
            return true;
        }

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                MOpcode mop = (inst->op == PIR_OP_FADD) ? MI_A64_VFADD
                            : (inst->op == PIR_OP_FSUB) ? MI_A64_VFSUB
                            : (inst->op == PIR_OP_FMUL) ? MI_A64_VFMUL : MI_A64_VFDIV;
                MInst v = minst_make(mop, pir_type_vreg_bytes(inst->ty), loc);
                v.arr = pir_vec_to_arr(inst->ty.u.vec);
                v.op_count = 3u; v.ops[0] = mop_vreg_def(dst0); v.ops[1] = mop_vreg_use(a); v.ops[2] = mop_vreg_use(b);
                emit(c, v);
            } else {
                MOpcode mop = (inst->op == PIR_OP_FADD) ? MI_A64_FADD
                            : (inst->op == PIR_OP_FSUB) ? MI_A64_FSUB
                            : (inst->op == PIR_OP_FMUL) ? MI_A64_FMUL : MI_A64_FDIV;
                MInst f = minst_make(mop, pir_type_vreg_bytes(inst->ty), loc);
                f.op_count = 3u; f.ops[0] = mop_vreg_def(dst0); f.ops[1] = mop_vreg_use(a); f.ops[2] = mop_vreg_use(b);
                emit(c, f);
            }
            return true;
        }

        case PIR_OP_FNEG: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                MInst v = minst_make(MI_A64_VFNEG, pir_type_vreg_bytes(inst->ty), loc);
                v.arr = pir_vec_to_arr(inst->ty.u.vec);
                v.op_count = 2u; v.ops[0] = mop_vreg_def(dst0); v.ops[1] = mop_vreg_use(a);
                emit(c, v);
            } else {
                MInst f = minst_make(MI_A64_FNEG, pir_type_vreg_bytes(inst->ty), loc);
                f.op_count = 2u; f.ops[0] = mop_vreg_def(dst0); f.ops[1] = mop_vreg_use(a);
                emit(c, f);
            }
            return true;
        }

        case PIR_OP_CMP_EQ:   case PIR_OP_CMP_NE:
        case PIR_OP_CMP_LT_S: case PIR_OP_CMP_LT_U:
        case PIR_OP_CMP_LE_S: case PIR_OP_CMP_LE_U:
        case PIR_OP_CMP_GT_S: case PIR_OP_CMP_GT_U:
        case PIR_OP_CMP_GE_S: case PIR_OP_CMP_GE_U: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);
                emit_vec_cmp(c, inst->op, dst0, a, b, inst->ty.u.vec, loc);
                return true;
            }
            emit_scalar_int_cmp_flags(c, inst, loc);
            MInst cs = minst_make(MI_A64_CSET, 4u, loc);
            cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst0); cs.ops[1] = mop_cond(int_cmp_to_cond(inst->op));
            emit(c, cs);
            return true;
        }

        case PIR_OP_FCMP_OEQ: case PIR_OP_FCMP_ONE: case PIR_OP_FCMP_OLT:
        case PIR_OP_FCMP_OLE: case PIR_OP_FCMP_OGT: case PIR_OP_FCMP_OGE:
        case PIR_OP_FCMP_UEQ: case PIR_OP_FCMP_UNE: case PIR_OP_FCMP_ULT:
        case PIR_OP_FCMP_ULE: case PIR_OP_FCMP_UGT: case PIR_OP_FCMP_UGE: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                emit_vec_cmp(c, inst->op, dst0, a, b, inst->ty.u.vec, loc);
                return true;
            }
            uint8_t fw = pir_type_vreg_bytes(inst->ty);
            MInst fc = minst_make(MI_A64_FCMP, fw, loc);
            fc.op_count = 2u; fc.ops[0] = mop_vreg_use(a); fc.ops[1] = mop_vreg_use(b);
            emit(c, fc);
            MCond sc;
            if (fcmp_single_cond(inst->op, &sc)) {
                MInst cs = minst_make(MI_A64_CSET, 4u, loc);
                cs.op_count = 2u; cs.ops[0] = mop_vreg_def(dst0); cs.ops[1] = mop_cond(sc);
                emit(c, cs);
            } else if (inst->op == PIR_OP_FCMP_ONE) {
                /* Ordered AND not equal: (mi || gt) */
                MVReg t1 = new_gp(c, 4u), t2 = new_gp(c, 4u);
                MInst c1 = minst_make(MI_A64_CSET, 4u, loc);
                c1.op_count = 2u; c1.ops[0] = mop_vreg_def(t1); c1.ops[1] = mop_cond(MCOND_MI);
                emit(c, c1);
                MInst c2 = minst_make(MI_A64_CSET, 4u, loc);
                c2.op_count = 2u; c2.ops[0] = mop_vreg_def(t2); c2.ops[1] = mop_cond(MCOND_GT);
                emit(c, c2);
                MInst o = minst_make(MI_A64_ORR_RR, 4u, loc);
                o.op_count = 3u; o.ops[0] = mop_vreg_def(dst0); o.ops[1] = mop_vreg_use(t1); o.ops[2] = mop_vreg_use(t2);
                emit(c, o);
            } else {
                /* UEQ: unordered OR equal (eq || vs) */
                MVReg t1 = new_gp(c, 4u), t2 = new_gp(c, 4u);
                MInst c1 = minst_make(MI_A64_CSET, 4u, loc);
                c1.op_count = 2u; c1.ops[0] = mop_vreg_def(t1); c1.ops[1] = mop_cond(MCOND_EQ);
                emit(c, c1);
                MInst c2 = minst_make(MI_A64_CSET, 4u, loc);
                c2.op_count = 2u; c2.ops[0] = mop_vreg_def(t2); c2.ops[1] = mop_cond(MCOND_VS);
                emit(c, c2);
                MInst o = minst_make(MI_A64_ORR_RR, 4u, loc);
                o.op_count = 3u; o.ops[0] = mop_vreg_def(dst0); o.ops[1] = mop_vreg_use(t1); o.ops[2] = mop_vreg_use(t2);
                emit(c, o);
            }
            return true;
        }

        case PIR_OP_SELECT: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirInstId fcid = PIR_INVALID_ID;
                MOpcode fmop = MI_NOP;
                PirOperand fa, fb;
                if (try_match_vec_fminmax(c->fn, c->val_uses, cur_pir_blk, inst, &fcid, &fmop, &fa, &fb) &&
                    c->inst_fused[fcid]) {
                    MVReg va = mat_op(c, &fa, loc);
                    MVReg vb = mat_op(c, &fb, loc);
                    MInst v = minst_make(fmop, pir_type_vreg_bytes(inst->ty), loc);
                    v.arr = pir_vec_to_arr(inst->ty.u.vec);
                    v.op_count = 3u;
                    v.ops[0] = mop_vreg_def(dst0);
                    v.ops[1] = mop_vreg_use(va);
                    v.ops[2] = mop_vreg_use(vb);
                    emit(c, v);
                    return true;
                }
                MVReg cond = mat_op(c, &inst->operands[0], loc);
                MVReg tv   = mat_op(c, &inst->operands[1], loc);
                MVReg fv   = mat_op(c, &inst->operands[2], loc);
                emit_vec_select(c, dst0, cond, tv, fv, pir_type_vreg_bytes(inst->ty), loc);
                return true;
            }
            uint32_t before_mat = c->mf->blocks.data[c->cur_mblk].insts.len;
            MVReg tv   = mat_op(c, &inst->operands[1], loc);
            MVReg fv   = mat_op(c, &inst->operands[2], loc);
            bool mat_clean = (c->mf->blocks.data[c->cur_mblk].insts.len == before_mat);
            MCond sel_cond = MCOND_NE;
            bool fused_cmp = false;
            PirInstId next_flags_id = PIR_INVALID_ID;
            if (inst->operands[0].kind == PIR_OPER_VAL) {
                PirValueId cv = inst->operands[0].u.val;
                const PirValueDef *vd = &c->fn->values.data[cv];
                if (vd->def_inst != PIR_INVALID_ID && vd->def_block == cur_pir_blk &&
                    c->inst_fused[vd->def_inst]) {
                    const PirInst *ci = &c->fn->insts.data[vd->def_inst];
                    bool flags_live = mat_clean && (c->last_flags_cmp_id == vd->def_inst);
                    if (ci->op >= PIR_OP_CMP_EQ && ci->op <= PIR_OP_CMP_GE_U) {
                        if (!flags_live) emit_scalar_int_cmp_flags(c, ci, loc);
                        sel_cond = int_cmp_to_cond(ci->op);
                        fused_cmp = true;
                        next_flags_id = vd->def_inst;
                    } else if (ci->op >= PIR_OP_FCMP_OEQ && ci->op <= PIR_OP_FCMP_UGE &&
                               fcmp_single_cond(ci->op, &sel_cond)) {
                        if (!flags_live) {
                            MVReg fa = mat_op(c, &ci->operands[0], loc);
                            MVReg fb = mat_op(c, &ci->operands[1], loc);
                            MInst fc = minst_make(MI_A64_FCMP, pir_type_vreg_bytes(ci->ty), loc);
                            fc.op_count = 2u; fc.ops[0] = mop_vreg_use(fa); fc.ops[1] = mop_vreg_use(fb);
                            emit(c, fc);
                        }
                        fused_cmp = true;
                        next_flags_id = vd->def_inst;
                    }
                }
            }
            if (!fused_cmp) {
                MVReg cond = mat_op(c, &inst->operands[0], loc);
                PirType ti8 = pir_type_scalar(PIR_SCALAR_I8);
                MVReg cn = is_already_zext_subword(c->fn, &inst->operands[0], ti8)
                           ? cond : normalize_int(c, cond, ti8, false, loc);
                MInst cmp = minst_make(MI_A64_CMP_RI, 4u, loc);
                cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(cn); cmp.ops[1] = mop_imm(0);
                emit(c, cmp);
            }
            c->last_flags_cmp_id = next_flags_id;
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            if (pir_type_rclass(inst->ty) == MREG_CLASS_FP) {
                MInst cs = minst_make(MI_A64_FCSEL, w, loc);
                cs.op_count = 4u;
                cs.ops[0] = mop_vreg_def(dst0); cs.ops[1] = mop_vreg_use(tv);
                cs.ops[2] = mop_vreg_use(fv);   cs.ops[3] = mop_cond(sel_cond);
                emit(c, cs);
            } else {
                MInst cs = minst_make(MI_A64_CSEL, w, loc);
                cs.op_count = 4u;
                cs.ops[0] = mop_vreg_def(dst0); cs.ops[1] = mop_vreg_use(tv);
                cs.ops[2] = mop_vreg_use(fv);   cs.ops[3] = mop_cond(sel_cond);
                emit(c, cs);
            }
            return true;
        }

        case PIR_OP_ZEXT:
        case PIR_OP_SEXT:
        case PIR_OP_TRUNC: {
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                /* Generic lane-wise vector integer cast */
                PirVecType svt = src_ty.u.vec, dvt = inst->ty.u.vec;
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                uint32_t lanes = pir_vec_lanes(dvt);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                MVReg acc = MVREG_NONE;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_gp(c, pir_type_vreg_bytes(sel_ty));
                    emit_vec_lane_extract(c, ls, src, svt, l, loc);
                    MVReg ld = new_gp(c, pir_type_vreg_bytes(del_ty));
                    if (inst->op == PIR_OP_SEXT) {
                        uint8_t dw = pir_type_vreg_bytes(del_ty);
                        MOpcode eop = (sel_ty.u.scalar == PIR_SCALAR_I8) ? MI_A64_SXTB
                                    : (sel_ty.u.scalar == PIR_SCALAR_I16) ? MI_A64_SXTH : MI_A64_SXTW;
                        MInst e = minst_make(eop, dw, loc);
                        e.op_count = 2u; e.ops[0] = mop_vreg_def(ld); e.ops[1] = mop_vreg_use(ls);
                        emit(c, e);
                    } else if (inst->op == PIR_OP_ZEXT) {
                        MVReg z = normalize_int(c, ls, sel_ty, false, loc);
                        MInst mv = minst_make(MI_A64_MOV_RR, 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(ld); mv.ops[1] = mop_vreg_use(z);
                        emit(c, mv);
                    } else {
                        MInst mv = minst_make(MI_A64_MOV_RR, 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(ld); mv.ops[1] = mop_vreg_use(ls);
                        emit(c, mv);
                    }
                    MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, dvb);
                    if (l == 0u) emit_vec_splat_scalar(c, nxt, ld, dvt, loc);
                    else         emit_vec_lane_insert(c, nxt, acc, ld, dvt, l, loc);
                    acc = nxt;
                }
                return true;
            }
            uint8_t dw = pir_type_vreg_bytes(inst->ty);
            if (inst->op == PIR_OP_TRUNC) {
                MInst mv = minst_make(MI_COPY, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            } else if (inst->op == PIR_OP_ZEXT) {
                if (src_ty.u.scalar == PIR_SCALAR_I8 || src_ty.u.scalar == PIR_SCALAR_I16) {
                    if (is_already_zext_subword(c->fn, &inst->operands[0], src_ty)) {
                        MInst mv = minst_make(MI_COPY, dw, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
                        emit(c, mv);
                    } else {
                        MInst e = minst_make(src_ty.u.scalar == PIR_SCALAR_I8 ? MI_A64_UXTB : MI_A64_UXTH, 4u, loc);
                        e.op_count = 2u; e.ops[0] = mop_vreg_def(dst0); e.ops[1] = mop_vreg_use(src);
                        emit(c, e);
                    }
                } else {
                    /* i32 -> i64 zero-ext: `mov w_dst, w_src` clears upper 32 bits of x_dst! */
                    MInst mv = minst_make(MI_A64_MOV_RR, 4u, loc);
                    mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
                    emit(c, mv);
                }
            } else {
                /* SEXT */
                MOpcode eop = (src_ty.u.scalar == PIR_SCALAR_I8)  ? MI_A64_SXTB
                            : (src_ty.u.scalar == PIR_SCALAR_I16) ? MI_A64_SXTH : MI_A64_SXTW;
                MInst e = minst_make(eop, dw, loc);
                e.op_count = 2u; e.ops[0] = mop_vreg_def(dst0); e.ops[1] = mop_vreg_use(src);
                emit(c, e);
            }
            return true;
        }

        case PIR_OP_FPEXT:
        case PIR_OP_FPTRUNC: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType svt = inst->operands[0].type.u.vec, dvt = inst->ty.u.vec;
                uint32_t lanes = pir_vec_lanes(dvt);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint8_t sw  = (inst->op == PIR_OP_FPEXT) ? 4u : 8u;
                uint8_t dw  = (inst->op == PIR_OP_FPEXT) ? 8u : 4u;
                MVReg acc = MVREG_NONE;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_fp(c, sw), ld = new_fp(c, dw);
                    emit_vec_lane_extract(c, ls, src, svt, l, loc);
                    MInst cv = minst_make(MI_A64_FCVT, dw, loc);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(ld); cv.ops[1] = mop_vreg_use(ls);
                    emit(c, cv);
                    MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, dvb);
                    if (l == 0u) emit_vec_splat_scalar(c, nxt, ld, dvt, loc);
                    else         emit_vec_lane_insert(c, nxt, acc, ld, dvt, l, loc);
                    acc = nxt;
                }
                return true;
            }
            MInst cv = minst_make(MI_A64_FCVT, pir_type_vreg_bytes(inst->ty), loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(src);
            emit(c, cv);
            return true;
        }

        case PIR_OP_SITOFP:
        case PIR_OP_UITOFP: {
            bool is_s = (inst->op == PIR_OP_SITOFP);
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType svt = src_ty.u.vec, dvt = inst->ty.u.vec;
                uint32_t sbits = pir_scalar_bits(pir_vec_elem_scalar(svt), 8u);
                uint32_t dbits = pir_scalar_bits(pir_vec_elem_scalar(dvt), 8u);
                if (sbits == dbits && (sbits == 32u || sbits == 64u)) {
                    MInst cv = minst_make(is_s ? MI_A64_VSCVTF : MI_A64_VUCVTF,
                                          pir_type_vreg_bytes(inst->ty), loc);
                    cv.arr = pir_vec_to_arr(dvt);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(src);
                    emit(c, cv);
                    return true;
                }
                /* Lane-wise fallback for mismatched element widths */
                uint32_t lanes = pir_vec_lanes(dvt);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                uint8_t dw = (uint8_t)(dbits / 8u);
                MVReg acc = MVREG_NONE;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_gp(c, pir_type_vreg_bytes(sel_ty));
                    emit_vec_lane_extract(c, ls, src, svt, l, loc);
                    ls = normalize_int(c, ls, sel_ty, is_s, loc);
                    MVReg ld = new_fp(c, dw);
                    MInst cv = minst_make(is_s ? MI_A64_SCVTF : MI_A64_UCVTF, dw, loc);
                    cv.aux_width = pir_type_vreg_bytes(sel_ty);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(ld); cv.ops[1] = mop_vreg_use(ls);
                    emit(c, cv);
                    MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, dvb);
                    if (l == 0u) emit_vec_splat_scalar(c, nxt, ld, dvt, loc);
                    else         emit_vec_lane_insert(c, nxt, acc, ld, dvt, l, loc);
                    acc = nxt;
                }
                return true;
            }
            MVReg ns = normalize_int(c, src, src_ty, is_s, loc);
            MInst cv = minst_make(is_s ? MI_A64_SCVTF : MI_A64_UCVTF, pir_type_vreg_bytes(inst->ty), loc);
            cv.aux_width = pir_type_vreg_bytes(src_ty);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(ns);
            emit(c, cv);
            return true;
        }

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI: {
            bool is_s = (inst->op == PIR_OP_FPTOSI);
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType svt = src_ty.u.vec, dvt = inst->ty.u.vec;
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                uint32_t sbits = pir_scalar_bits(sel_ty.u.scalar, 8u);
                uint32_t dbits = pir_scalar_bits(del_ty.u.scalar, 8u);
                if (sbits == dbits && (sbits == 32u || sbits == 64u) &&
                    (inst->flags & PIR_FLAG_TRAP) == 0u) {
                    MInst cv = minst_make(is_s ? MI_A64_VFCVTZS : MI_A64_VFCVTZU,
                                          pir_type_vreg_bytes(inst->ty), loc);
                    cv.arr = pir_vec_to_arr(dvt);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(src);
                    emit(c, cv);
                    return true;
                }
                uint32_t lanes = pir_vec_lanes(dvt);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint8_t sw  = (uint8_t)(sbits / 8u);
                uint8_t dw  = (dbits == 64u) ? 8u : 4u;
                MVReg acc = MVREG_NONE;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_fp(c, sw), ld = new_gp(c, dw);
                    emit_vec_lane_extract(c, ls, src, svt, l, loc);
                    emit_scalar_fptoi_regs(c, is_s, sel_ty, del_ty, inst->flags, ls, ld, loc);
                    MVReg nxt = (l + 1u == lanes) ? dst0 : new_fp(c, dvb);
                    if (l == 0u) emit_vec_splat_scalar(c, nxt, ld, dvt, loc);
                    else         emit_vec_lane_insert(c, nxt, acc, ld, dvt, l, loc);
                    acc = nxt;
                }
                return true;
            }
            emit_scalar_fptoi_regs(c, is_s, src_ty, inst->ty, inst->flags, src, dst0, loc);
            return true;
        }

        case PIR_OP_BITCAST: {
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MRegClass src_rc = pir_type_rclass(src_ty);
            MRegClass dst_rc = pir_type_rclass(inst->ty);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            if (src_rc == dst_rc) {
                MInst cp = minst_make(MI_COPY, w, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(src);
                emit(c, cp);
            } else if (src_rc == MREG_CLASS_GP && dst_rc == MREG_CLASS_FP) {
                MInst fm = minst_make(MI_A64_FMOV_G2F, w, loc);
                fm.op_count = 2u; fm.ops[0] = mop_vreg_def(dst0); fm.ops[1] = mop_vreg_use(src);
                emit(c, fm);
            } else {
                MInst fm = minst_make(MI_A64_FMOV_F2G, w, loc);
                fm.op_count = 2u; fm.ops[0] = mop_vreg_def(dst0); fm.ops[1] = mop_vreg_use(src);
                emit(c, fm);
            }
            return true;
        }

        case PIR_OP_PTRTOINT: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst mv = minst_make(MI_A64_MOV_RR, pir_type_vreg_bytes(inst->ty), loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
            emit(c, mv);
            return true;
        }

        case PIR_OP_INTTOPTR: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            uint8_t sw = pir_type_vreg_bytes(inst->operands[0].type);
            MInst mv = minst_make(MI_A64_MOV_RR, sw, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
            emit(c, mv);
            return true;
        }

        case PIR_OP_SPLAT: {
            PirVecType vt = inst->ty.u.vec;
            MVecArr arr = pir_vec_to_arr(vt);
            uint8_t vb  = (uint8_t)(pir_vec_bits(vt) / 8u);
            MVReg s = mat_op(c, &inst->operands[0], loc);
            MOpcode mop = pir_scalar_is_float(pir_vec_elem_scalar(vt)) ? MI_A64_VDUP_LANE : MI_A64_VDUP_GP;
            MInst dp = minst_make(mop, vb, loc);
            dp.arr = arr; dp.op_count = 2u;
            dp.ops[0] = mop_vreg_def(dst0); dp.ops[1] = mop_vreg_use(s);
            emit(c, dp);
            return true;
        }

        case PIR_OP_EXTRACT: {
            MVReg v = mat_op(c, &inst->operands[0], loc);
            emit_vec_lane_extract(c, dst0, v, inst->ty.u.vec, (uint32_t)inst->imm_u64, loc);
            return true;
        }

        case PIR_OP_INSERT: {
            MVReg v = mat_op(c, &inst->operands[0], loc);
            MVReg s = mat_op(c, &inst->operands[1], loc);
            emit_vec_lane_insert(c, dst0, v, s, inst->ty.u.vec, (uint32_t)inst->imm_u64, loc);
            return true;
        }

        case PIR_OP_SHUFFLE: {
            PirVecType vt = inst->ty.u.vec;
            MVecArr arr = pir_vec_to_arr(vt);
            uint32_t lanes = pir_vec_lanes(vt);
            uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
            MVReg va = mat_op(c, &inst->operands[0], loc);
            MVReg vb_r = mat_op(c, &inst->operands[1], loc);
            /* Build into fresh temp register to avoid aliasing va or vb_r, then copy to dst0. */
            MVReg tmp = new_fp(c, vb);
            /* Initialize tmp from va so regalloc sees a clean def before defuse ins sequence */
            MInst init = minst_make(MI_COPY, vb, loc);
            init.op_count = 2u; init.ops[0] = mop_vreg_def(tmp); init.ops[1] = mop_vreg_use(va);
            emit(c, init);
            for (uint32_t l = 0u; l < lanes; ++l) {
                uint8_t m = inst->shuffle_mask[l];
                MVReg src_v = (m < lanes) ? va : vb_r;
                uint8_t src_lane = (m < lanes) ? m : (uint8_t)(m - lanes);
                MInst ins = minst_make(MI_A64_VINS_LANE, vb, loc);
                ins.arr = arr; ins.aux_width = (uint8_t)l; ins.aux_lane2 = src_lane;
                ins.op_count = 2u; ins.ops[0] = mop_vreg_defuse(tmp); ins.ops[1] = mop_vreg_use(src_v);
                emit(c, ins);
            }
            MInst out = minst_make(MI_COPY, vb, loc);
            out.op_count = 2u; out.ops[0] = mop_vreg_def(dst0); out.ops[1] = mop_vreg_use(tmp);
            emit(c, out);
            return true;
        }

        case PIR_OP_ALLOCA_PTR: {
            uint32_t sid = mfunc_add_slot(c->mf, inst->imm_u64, inst->align, false);
            MInst fa = minst_make(MI_FRAME_ADDR, 8u, loc);
            fa.op_count = 3u;
            fa.ops[0] = mop_vreg_def(dst0);
            fa.ops[1] = mop_slot(sid);
            fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return true;
        }

        case PIR_OP_ALLOCA_DYN: {
            c->mf->has_alloca_dyn = true;
            MVReg sz = mat_op(c, &inst->operands[0], loc);
            MVReg sz64 = sz;
            if (inst->operands[0].type.u.scalar == PIR_SCALAR_I32) {
                sz64 = new_gp(c, 8u);
                MInst zx = minst_make(MI_A64_MOV_RR, 4u, loc);
                zx.op_count = 2u; zx.ops[0] = mop_vreg_def(sz64); zx.ops[1] = mop_vreg_use(sz);
                emit(c, zx);
            }
            uint64_t al = inst->align > 16u ? (uint64_t)inst->align : 16u;
            /* Round size up to al: (sz64 + al - 1) & ~(al - 1) */
            MVReg r1 = new_gp(c, 8u), r2 = new_gp(c, 8u), msk = new_gp(c, 8u);
            if (al - 1u <= 4095u) {
                MInst ad = minst_make(MI_A64_ADD_RI, 8u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(r1); ad.ops[1] = mop_vreg_use(sz64);
                ad.ops[2] = mop_imm((int64_t)(al - 1u));
                emit(c, ad);
            } else {
                MVReg am = new_gp(c, 8u);
                emit_imm_gp(c, am, al - 1u, 8u, loc);
                MInst ad = minst_make(MI_A64_ADD_RR, 8u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(r1); ad.ops[1] = mop_vreg_use(sz64); ad.ops[2] = mop_vreg_use(am);
                emit(c, ad);
            }
            emit_imm_gp(c, msk, ~(al - 1u), 8u, loc);
            MInst an = minst_make(MI_A64_AND_RR, 8u, loc);
            an.op_count = 3u; an.ops[0] = mop_vreg_def(r2); an.ops[1] = mop_vreg_use(r1); an.ops[2] = mop_vreg_use(msk);
            emit(c, an);

            MVReg cur_sp = new_gp(c, 8u), new_sp = new_gp(c, 8u);
            MInst gsp = minst_make(MI_A64_MOV_SP, 8u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(cur_sp);
            emit(c, gsp);
            MInst sb = minst_make(MI_A64_SUB_RR, 8u, loc);
            sb.op_count = 3u; sb.ops[0] = mop_vreg_def(new_sp); sb.ops[1] = mop_vreg_use(cur_sp); sb.ops[2] = mop_vreg_use(r2);
            emit(c, sb);
            if (al > 16u) {
                MVReg al_sp = new_gp(c, 8u);
                MInst an2 = minst_make(MI_A64_AND_RR, 8u, loc);
                an2.op_count = 3u; an2.ops[0] = mop_vreg_def(al_sp); an2.ops[1] = mop_vreg_use(new_sp); an2.ops[2] = mop_vreg_use(msk);
                emit(c, an2);
                new_sp = al_sp;
            }
            MInst ssp = minst_make(MI_A64_MOV_SP, 8u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(new_sp);
            emit(c, ssp);
            MInst cp = minst_make(MI_COPY, 8u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(new_sp);
            emit(c, cp);
            return true;
        }

        case PIR_OP_STACKSAVE: {
            c->mf->has_alloca_dyn = true;
            MInst gsp = minst_make(MI_A64_MOV_SP, 8u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(dst0);
            emit(c, gsp);
            return true;
        }

        case PIR_OP_STACKRESTORE: {
            c->mf->has_alloca_dyn = true;
            MVReg p = mat_op(c, &inst->operands[0], loc);
            MInst ssp = minst_make(MI_A64_MOV_SP, 8u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(p);
            emit(c, ssp);
            return true;
        }

        case PIR_OP_PTR_ADD: {
            MVReg base = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                int64_t off = (inst->ty.u.scalar == PIR_SCALAR_I32)
                              ? (int64_t)(int32_t)eval_imm_u64(&inst->operands[1])
                              : (int64_t)eval_imm_u64(&inst->operands[1]);
                if (off == 0) {
                    MInst cp = minst_make(MI_COPY, 8u, loc);
                    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_vreg_use(base);
                    emit(c, cp);
                    return true;
                }
                if (off > 0 && off <= 4095) {
                    MInst ad = minst_make(MI_A64_ADD_RI, 8u, loc);
                    ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst0); ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_imm(off);
                    emit(c, ad);
                    return true;
                }
                if (off < 0 && -off <= 4095) {
                    MInst sb = minst_make(MI_A64_SUB_RI, 8u, loc);
                    sb.op_count = 3u; sb.ops[0] = mop_vreg_def(dst0); sb.ops[1] = mop_vreg_use(base); sb.ops[2] = mop_imm(-off);
                    emit(c, sb);
                    return true;
                }
                MVReg oi = new_gp(c, 8u);
                emit_imm_gp(c, oi, (uint64_t)off, 8u, loc);
                MInst ad = minst_make(MI_A64_ADD_RR, 8u, loc);
                ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst0); ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(oi);
                emit(c, ad);
                return true;
            }
            MVReg idx = mat_op(c, &inst->operands[1], loc);
            if (inst->ty.u.scalar == PIR_SCALAR_I32) {
                /* ptr_add.i32 sign-extends the i32 byte offset */
                MVReg sx = new_gp(c, 8u);
                MInst e = minst_make(MI_A64_SXTW, 8u, loc);
                e.op_count = 2u; e.ops[0] = mop_vreg_def(sx); e.ops[1] = mop_vreg_use(idx);
                emit(c, e);
                idx = sx;
            }
            MInst ad = minst_make(MI_A64_ADD_RR, 8u, loc);
            ad.op_count = 3u; ad.ops[0] = mop_vreg_def(dst0); ad.ops[1] = mop_vreg_use(base); ad.ops[2] = mop_vreg_use(idx);
            emit(c, ad);
            return true;
        }

        case PIR_OP_LOAD: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg base = MVREG_NONE, idx = MVREG_NONE;
            int64_t off = 0;
            bool has_idx = false, use_sxtw = false;
            uint8_t sh = 0u;
            try_match_mem_addr(c, &inst->operands[0], mb, &base, &off, &has_idx, &idx, &sh, &use_sxtw, loc);
            if (has_idx) {
                MInst ld = minst_make(MI_A64_LDR_REG, mb, loc);
                ld.aux_width = sh;
                ld.aux_lane2 = use_sxtw ? 1u : 0u;
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_vreg_use(idx);
                emit(c, ld);
            } else {
                MInst ld = minst_make(MI_A64_LDR, mb, loc);
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_imm(off);
                emit(c, ld);
            }
            return true;
        }

        case PIR_OP_STORE: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg val  = mat_op(c, &inst->operands[0], loc);
            MVReg base = MVREG_NONE, idx = MVREG_NONE;
            int64_t off = 0;
            bool has_idx = false, use_sxtw = false;
            uint8_t sh = 0u;
            try_match_mem_addr(c, &inst->operands[1], mb, &base, &off, &has_idx, &idx, &sh, &use_sxtw, loc);
            if (has_idx) {
                MInst st = minst_make(MI_A64_STR_REG, mb, loc);
                st.aux_width = sh;
                st.aux_lane2 = use_sxtw ? 1u : 0u;
                st.op_count = 3u; st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(base); st.ops[2] = mop_vreg_use(idx);
                emit(c, st);
            } else {
                MInst st = minst_make(MI_A64_STR, mb, loc);
                st.op_count = 3u; st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(base); st.ops[2] = mop_imm(off);
                emit(c, st);
            }
            return true;
        }

        case PIR_OP_ATOMIC_LOAD: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            if (inst->order == PIR_ORDER_RELAXED) {
                MInst ld = minst_make(MI_A64_LDR, mb, loc);
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
                emit(c, ld);
            } else {
                MInst ld = minst_make(MI_A64_LDAR, mb, loc);
                ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(ptr);
                emit(c, ld);
            }
            return true;
        }

        case PIR_OP_ATOMIC_STORE: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            if (inst->order == PIR_ORDER_RELAXED) {
                MInst st = minst_make(MI_A64_STR, mb, loc);
                st.op_count = 3u; st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
                emit(c, st);
            } else {
                MInst st = minst_make(MI_A64_STLR, mb, loc);
                st.op_count = 2u; st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(ptr);
                emit(c, st);
            }
            return true;
        }

        case PIR_OP_ATOMIC_RMW: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            uint8_t w  = pir_type_vreg_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);
            /* Pin operands to caller-saved GP regs (x9..x11) so they never spill into
             * x16/x17 which MI_A64_ATOMIC_RMW uses internally for its LL/SC loop. */
            MVReg p_old = new_gp(c, w);  c->mf->vregs.data[p_old].fixed_preg = A64_GP(9u);
            MVReg p_ptr = new_gp(c, 8u); c->mf->vregs.data[p_ptr].fixed_preg = A64_GP(10u);
            MVReg p_val = new_gp(c, w);  c->mf->vregs.data[p_val].fixed_preg = A64_GP(11u);

            MInst cp1 = minst_make(MI_COPY, 8u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr);
            emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, w, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_val); cp2.ops[1] = mop_vreg_use(val);
            emit(c, cp2);

            MInst rmw = minst_make(MI_A64_ATOMIC_RMW, mb, loc);
            rmw.aux_id   = (uint32_t)inst->rmw_op;
            rmw.aux_id2  = (uint32_t)inst->order;
            rmw.op_count = 3u;
            rmw.ops[0]   = mop_vreg_def_ec(p_old);
            rmw.ops[1]   = mop_vreg_use(p_ptr);
            rmw.ops[2]   = mop_vreg_use(p_val);
            emit(c, rmw);

            MInst co = minst_make(MI_COPY, w, loc);
            co.op_count = 2u; co.ops[0] = mop_vreg_def(dst0); co.ops[1] = mop_vreg_use(p_old);
            emit(c, co);
            return true;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            uint8_t w  = pir_type_vreg_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg exp = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
            MVReg des = mat_op(c, &inst->operands[2], loc);
            /* Pin all 5 operands to distinct caller-saved GP regs (x9..x13) so high
             * register pressure can never alias early-clobber defs or clobber w16. */
            MVReg p_old = new_gp(c, w);  c->mf->vregs.data[p_old].fixed_preg = A64_GP(9u);
            MVReg p_ptr = new_gp(c, 8u); c->mf->vregs.data[p_ptr].fixed_preg = A64_GP(10u);
            MVReg p_exp = new_gp(c, w);  c->mf->vregs.data[p_exp].fixed_preg = A64_GP(11u);
            MVReg p_des = new_gp(c, w);  c->mf->vregs.data[p_des].fixed_preg = A64_GP(12u);
            MVReg p_ok  = new_gp(c, 4u); c->mf->vregs.data[p_ok].fixed_preg  = A64_GP(13u);

            MInst cp1 = minst_make(MI_COPY, 8u, loc);
            cp1.op_count = 2u; cp1.ops[0] = mop_vreg_def(p_ptr); cp1.ops[1] = mop_vreg_use(ptr);
            emit(c, cp1);
            MInst cp2 = minst_make(MI_COPY, w, loc);
            cp2.op_count = 2u; cp2.ops[0] = mop_vreg_def(p_exp); cp2.ops[1] = mop_vreg_use(exp);
            emit(c, cp2);
            MInst cp3 = minst_make(MI_COPY, w, loc);
            cp3.op_count = 2u; cp3.ops[0] = mop_vreg_def(p_des); cp3.ops[1] = mop_vreg_use(des);
            emit(c, cp3);

            MInst cas = minst_make(MI_A64_ATOMIC_CAS, mb, loc);
            cas.aux_id   = (uint32_t)inst->order;
            cas.aux_id2  = (uint32_t)inst->fail_order;
            cas.op_count = 5u;
            cas.ops[0]   = mop_vreg_def_ec(p_old);
            cas.ops[1]   = mop_vreg_def_ec(p_ok);
            cas.ops[2]   = mop_vreg_use(p_ptr);
            cas.ops[3]   = mop_vreg_use(p_exp);
            cas.ops[4]   = mop_vreg_use(p_des);
            emit(c, cas);

            MInst co1 = minst_make(MI_COPY, w, loc);
            co1.op_count = 2u; co1.ops[0] = mop_vreg_def(dst0); co1.ops[1] = mop_vreg_use(p_old);
            emit(c, co1);
            MInst co2 = minst_make(MI_COPY, 4u, loc);
            co2.op_count = 2u; co2.ops[0] = mop_vreg_def(dst1); co2.ops[1] = mop_vreg_use(p_ok);
            emit(c, co2);
            return true;
        }

        case PIR_OP_FENCE: {
            MInst f = minst_make(MI_A64_DMB, 0u, loc);
            f.aux_id = (uint32_t)inst->order;
            emit(c, f);
            return true;
        }

        case PIR_OP_VA_START: {
            MVReg ap = mat_op(c, &inst->operands[0], loc);
            MInst va = minst_make(MI_VA_START, 8u, loc);
            va.op_count = 1u; va.ops[0] = mop_vreg_use(ap);
            emit(c, va);
            return true;
        }

        case PIR_OP_CALL:
        case PIR_OP_CALL_INDIRECT:
            emit_call_inst(c, inst, loc);
            return true;

        case PIR_OP_JMP: {
            uint32_t pc_id = build_edge_pcopy(c, &inst->targets[0], loc);
            if (pc_id != UINT32_MAX) {
                MInst pc = minst_make(MI_PCOPY, 0u, loc);
                pc.aux_id = pc_id;
                emit(c, pc);
            }
            uint32_t dst_mblk = c->pir_to_mblk[inst->targets[0].target];
            MInst jmp = minst_make(MI_A64_B, 4u, loc);
            jmp.op_count = 1u; jmp.ops[0] = mop_block(dst_mblk);
            emit(c, jmp);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_mblk);
            return true;
        }

        case PIR_OP_BR: {
            uint32_t true_mblk  = prepare_branch_edge(c, &inst->targets[0], loc);
            uint32_t false_mblk = prepare_branch_edge(c, &inst->targets[1], loc);

            /* Check if condition came from a fused cmp_* / fcmp_* in the same block! */
            bool fused = false;
            if (inst->operands[0].kind == PIR_OPER_VAL) {
                PirValueId cv = inst->operands[0].u.val;
                const PirValueDef *vd = &c->fn->values.data[cv];
                if (vd->def_inst != PIR_INVALID_ID && vd->def_block == cur_pir_blk &&
                    c->inst_fused[vd->def_inst]) {
                    const PirInst *ci = &c->fn->insts.data[vd->def_inst];
                    if (ci->op >= PIR_OP_CMP_EQ && ci->op <= PIR_OP_CMP_GE_U) {
                        emit_scalar_int_cmp_flags(c, ci, loc);
                        MInst bc = minst_make(MI_A64_BCOND, 4u, loc);
                        bc.op_count = 2u; bc.ops[0] = mop_cond(int_cmp_to_cond(ci->op)); bc.ops[1] = mop_block(true_mblk);
                        emit(c, bc);
                        fused = true;
                    } else if (ci->op >= PIR_OP_ADD_OVF_S && ci->op <= PIR_OP_MUL_OVF_U) {
                        MVReg dv = (ci->results[0] != PIR_INVALID_ID) ? c->val_vreg[ci->results[0]] : new_gp(c, pir_type_vreg_bytes(ci->ty));
                        MCond ov_cond = emit_scalar_ovf_core(c, ci->op, ci->ty, &ci->operands[0], &ci->operands[1], dv, 0u, loc, false);
                        MInst bc = minst_make(MI_A64_BCOND, 4u, loc);
                        bc.op_count = 2u; bc.ops[0] = mop_cond(ov_cond); bc.ops[1] = mop_block(true_mblk);
                        emit(c, bc);
                        fused = true;
                    } else if (ci->op >= PIR_OP_FCMP_OEQ && ci->op <= PIR_OP_FCMP_UGE) {
                        MCond fc_cond;
                        if (fcmp_single_cond(ci->op, &fc_cond)) {
                            MVReg fa = mat_op(c, &ci->operands[0], loc);
                            MVReg fb = mat_op(c, &ci->operands[1], loc);
                            MInst fc = minst_make(MI_A64_FCMP, pir_type_vreg_bytes(ci->ty), loc);
                            fc.op_count = 2u; fc.ops[0] = mop_vreg_use(fa); fc.ops[1] = mop_vreg_use(fb);
                            emit(c, fc);
                            MInst bc = minst_make(MI_A64_BCOND, 4u, loc);
                            bc.op_count = 2u; bc.ops[0] = mop_cond(fc_cond); bc.ops[1] = mop_block(true_mblk);
                            emit(c, bc);
                            fused = true;
                        }
                    }
                }
            }
            if (!fused) {
                MVReg cond = normalize_int(c, mat_op(c, &inst->operands[0], loc),
                                           pir_type_scalar(PIR_SCALAR_I8), false, loc);
                MInst cb = minst_make(MI_A64_CBNZ, 4u, loc);
                cb.op_count = 2u; cb.ops[0] = mop_vreg_use(cond); cb.ops[1] = mop_block(true_mblk);
                emit(c, cb);
            }
            MInst bf = minst_make(MI_A64_B, 4u, loc);
            bf.op_count = 1u; bf.ops[0] = mop_block(false_mblk);
            emit(c, bf);
            mfunc_add_edge(c->mf, c->cur_mblk, true_mblk);
            mfunc_add_edge(c->mf, c->cur_mblk, false_mblk);
            return true;
        }

        case PIR_OP_SWITCH: {
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            uint64_t type_mask = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? UINT64_C(0xFF)
                               : (inst->ty.u.scalar == PIR_SCALAR_I16) ? UINT64_C(0xFFFF)
                               : (inst->ty.u.scalar == PIR_SCALAR_I32) ? UINT64_C(0xFFFFFFFF)
                                                                       : ~UINT64_C(0);
            int64_t signed_max = (inst->ty.u.scalar == PIR_SCALAR_I8)  ? INT64_C(127)
                               : (inst->ty.u.scalar == PIR_SCALAR_I16) ? INT64_C(32767)
                               : (inst->ty.u.scalar == PIR_SCALAR_I32) ? INT64_C(2147483647)
                                                                       : INT64_MAX;
            uint32_t def_mblk = prepare_branch_edge(c, &inst->targets[0], loc);
            uint32_t nc = inst->switch_case_count;

            uint32_t *case_mblks = nc ? (uint32_t *)calloc(nc, sizeof(uint32_t)) : NULL;
            if (nc && !case_mblks) abort();
            for (uint32_t i = 0u; i < nc; ++i) {
                case_mblks[i] = prepare_branch_edge(c, &inst->switch_cases[i].target, loc);
            }

            /* Decide whether to emit a PC-relative jump table (dense >= 4 cases). */
            bool use_jtable = false;
            int64_t min_v = 0, max_v = 0;
            if (nc >= 4u) {
                use_jtable = true;
                for (uint32_t i = 0u; i < nc; ++i) {
                    int64_t cv = inst->switch_cases[i].is_negative
                                 ? (int64_t)(UINT64_C(0) - inst->switch_cases[i].case_lo)
                                 : (int64_t)inst->switch_cases[i].case_lo;
                    if (i == 0u || cv < min_v) min_v = cv;
                    if (i == 0u || cv > max_v) max_v = cv;
                }
                uint64_t span = (uint64_t)max_v - (uint64_t)min_v;
                if (max_v < min_v || span > 255u ||
                    (span + 1u) > (uint64_t)nc * 4u ||
                    (min_v < 0 && max_v > signed_max)) {
                    use_jtable = false;
                }
            }

            MVReg val = normalize_int(c, mat_op(c, &inst->operands[0], loc),
                                      inst->ty, use_jtable && (min_v < 0), loc);

            if (use_jtable) {
                uint32_t nent = (uint32_t)((uint64_t)max_v - (uint64_t)min_v + 1u);
                uint32_t *tbl = (uint32_t *)malloc((size_t)nent * sizeof(uint32_t));
                if (!tbl) abort();
                for (uint32_t k = 0u; k < nent; ++k) tbl[k] = def_mblk;
                for (uint32_t i = 0u; i < nc; ++i) {
                    int64_t cv = inst->switch_cases[i].is_negative
                                 ? (int64_t)(UINT64_C(0) - inst->switch_cases[i].case_lo)
                                 : (int64_t)inst->switch_cases[i].case_lo;
                    tbl[(uint32_t)((uint64_t)cv - (uint64_t)min_v)] = case_mblks[i];
                }
                uint32_t jt_id = mfunc_add_jtable(c->mf, min_v, nent, tbl, def_mblk);
                free(tbl);

                /* Ensure value is sign/zero-extended to 64-bit if i8/i16/i32 switch */
                MVReg val64 = val;
                if (w == 4u) {
                    val64 = new_gp(c, 8u);
                    MInst sx = minst_make(min_v < 0 ? MI_A64_SXTW : MI_A64_MOV_RR,
                                          min_v < 0 ? 8u : 4u, loc);
                    sx.op_count = 2u; sx.ops[0] = mop_vreg_def(val64); sx.ops[1] = mop_vreg_use(val);
                    emit(c, sx);
                }
                MInst sw = minst_make(MI_A64_SWITCH_TBL, 8u, loc);
                sw.op_count = 3u;
                sw.ops[0] = mop_vreg_use(val64);
                sw.ops[1] = mop_jtable(jt_id);
                sw.ops[2] = mop_block(def_mblk);
                emit(c, sw);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mblk);
                for (uint32_t i = 0u; i < nc; ++i) {
                    mfunc_add_edge(c->mf, c->cur_mblk, case_mblks[i]);
                }
            } else {
                /* Linear compare-and-branch chain (mask case values to switch type width) */
                for (uint32_t i = 0u; i < nc; ++i) {
                    uint64_t cv = (inst->switch_cases[i].is_negative
                                   ? (UINT64_C(0) - inst->switch_cases[i].case_lo)
                                   : inst->switch_cases[i].case_lo) & type_mask;
                    if (cv <= 4095u) {
                        MInst cmp = minst_make(MI_A64_CMP_RI, w, loc);
                        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(val); cmp.ops[1] = mop_imm((int64_t)cv);
                        emit(c, cmp);
                    } else {
                        MVReg cv_r = new_gp(c, w);
                        emit_imm_gp(c, cv_r, cv, w, loc);
                        MInst cmp = minst_make(MI_A64_CMP_RR, w, loc);
                        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(val); cmp.ops[1] = mop_vreg_use(cv_r);
                        emit(c, cmp);
                    }
                    MInst bc = minst_make(MI_A64_BCOND, 4u, loc);
                    bc.op_count = 2u; bc.ops[0] = mop_cond(MCOND_EQ); bc.ops[1] = mop_block(case_mblks[i]);
                    emit(c, bc);
                    mfunc_add_edge(c->mf, c->cur_mblk, case_mblks[i]);
                }
                MInst bd = minst_make(MI_A64_B, 4u, loc);
                bd.op_count = 1u; bd.ops[0] = mop_block(def_mblk);
                emit(c, bd);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mblk);
            }
            free(case_mblks);
            return true;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && c->multi_ret_slot != UINT32_MAX) {
                const PirCabiRetInfo *cr = &c->fn->cabi_ret;
                MVReg base = new_gp(c, 8u);
                MInst fa = minst_make(MI_FRAME_ADDR, 8u, loc);
                fa.op_count = 3u;
                fa.ops[0] = mop_vreg_def(base);
                fa.ops[1] = mop_slot(c->multi_ret_slot);
                fa.ops[2] = mop_imm(0);
                emit(c, fa);

                MVReg rvs[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
                MPReg rregs[4] = { MREG_NONE, MREG_NONE, MREG_NONE, MREG_NONE };
                uint64_t off = 0u;
                for (uint32_t k = 0u; k < cr->reg_count; ++k) {
                    PirType rt = cr->regs[k];
                    uint8_t mb = pir_type_mem_bytes(rt);
                    rvs[k]   = new_vreg_for_type(c, rt);
                    rregs[k] = (pir_type_rclass(rt) == MREG_CLASS_FP) ? A64_FP(k) : A64_GP(k);
                    MInst ld = minst_make(MI_A64_LDR, mb, loc);
                    ld.op_count = 3u;
                    ld.ops[0] = mop_vreg_def(rvs[k]);
                    ld.ops[1] = mop_vreg_use(base);
                    ld.ops[2] = mop_imm((int64_t)off);
                    emit(c, ld);
                    off += mb;
                }
                for (uint32_t k = 0u; k < cr->reg_count; ++k) {
                    MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(cr->regs[k]), loc);
                    cp.op_count = 2u;
                    cp.ops[0] = mop_preg_def(rregs[k]);
                    cp.ops[1] = mop_vreg_use(rvs[k]);
                    emit(c, cp);
                }
                MInst r = minst_make(MI_A64_RET, 8u, loc);
                r.op_count = (uint8_t)cr->reg_count;
                for (uint32_t k = 0u; k < cr->reg_count; ++k) {
                    r.ops[k] = mop_preg_use(rregs[k]);
                }
                emit(c, r);
            } else if (inst->operand_count == 1u) {
                PirType rty = c->fn->ret_type;
                MVReg rv = mat_op(c, &inst->operands[0], loc);
                if (rty.kind == PIR_TYPE_SCALAR &&
                    (rty.u.scalar == PIR_SCALAR_I8 || rty.u.scalar == PIR_SCALAR_I16) &&
                    (c->fn->attrs & (PIR_FATTR_RET_SIGNEXT | PIR_FATTR_RET_ZEROEXT)) != 0u) {
                    rv = normalize_int(c, rv, rty, (c->fn->attrs & PIR_FATTR_RET_SIGNEXT) != 0u, loc);
                }
                MPReg rreg = (pir_type_rclass(rty) == MREG_CLASS_FP) ? A64_FP(0u) : A64_GP(0u);
                MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(rty), loc);
                cp.op_count = 2u; cp.ops[0] = mop_preg_def(rreg); cp.ops[1] = mop_vreg_use(rv);
                emit(c, cp);
                MInst r = minst_make(MI_A64_RET, pir_type_vreg_bytes(rty), loc);
                r.op_count = 1u; r.ops[0] = mop_preg_use(rreg);
                emit(c, r);
            } else {
                MInst r = minst_make(MI_A64_RET, 0u, loc);
                emit(c, r);
            }
            return true;
        }

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE: {
            MInst u = minst_make(MI_A64_UDF, 4u, loc);
            emit(c, u);
            return true;
        }

        /* HIR-only opcodes (forbidden in verified LIR) */
        case PIR_OP_ALLOCA:
        case PIR_OP_FIELD_PTR:
        case PIR_OP_ELEM_PTR:
        case PIR_OP_MEMCPY:
        case PIR_OP_MEMMOVE:
        case PIR_OP_MEMSET:
        case PIR_OP_VA_ARG:
        case PIR_OP_VA_ARG_BUF:
        case PIR_OP_VA_COPY:
        case PIR_OP_VA_END:
        case PIR_OP_INVOKE:
        case PIR_OP_INVOKE_INDIRECT:
        case PIR_OP_RESUME:
            pir_diag_error(c->diag, inst->src_line, 0u,
                           "ARM64 ISel encountered unlowered HIR opcode %u", (unsigned)inst->op);
            return false;
    }
    return false;
}

/* =========================================================================
 * Pre-Pass: Use Counts & Fusion Marking (cmp+br, ptr_add+load/store)
 * ========================================================================= */

static void count_uses_in_ops(const PirOperand *ops, uint32_t n, uint32_t *uses) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL) uses[ops[i].u.val]++;
    }
}

static void analyze_fusion(ISelCtx *c, const PirCfg *cfg) {
    const PirFunction *fn = c->fn;
    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        const PirBlock *blk = &fn->blocks.data[cfg->rpo[ri]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            count_uses_in_ops(ins->operands, ins->operand_count, c->val_uses);
            count_uses_in_ops(ins->targets[0].args, ins->targets[0].arg_count, c->val_uses);
            count_uses_in_ops(ins->targets[1].args, ins->targets[1].arg_count, c->val_uses);
            for (uint32_t k = 0u; k < ins->switch_case_count; ++k) {
                count_uses_in_ops(ins->switch_cases[k].target.args,
                                  ins->switch_cases[k].target.arg_count, c->val_uses);
            }
        }
    }

    /* Pre-pass for multi-use constant ptr_add(base, #imm) folding across loads/stores. */
    uint32_t *const_pa_ok_uses = (uint32_t *)calloc((size_t)fn->values.len + 1u, sizeof(uint32_t));
    if (!const_pa_ok_uses) abort();
    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        const PirBlock *blk = &fn->blocks.data[cfg->rpo[ri]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_LOAD || ins->op == PIR_OP_STORE) {
                const PirOperand *pop = (ins->op == PIR_OP_LOAD) ? &ins->operands[0] : &ins->operands[1];
                uint8_t mb = pir_type_mem_bytes(ins->ty);
                if (pop->kind == PIR_OPER_VAL) {
                    PirValueId pv = pop->u.val;
                    const PirValueDef *vd = &fn->values.data[pv];
                    if (vd->def_inst != PIR_INVALID_ID) {
                        const PirInst *pa = &fn->insts.data[vd->def_inst];
                        if (pa->op == PIR_OP_PTR_ADD &&
                            pa->operands[0].kind == PIR_OPER_VAL &&
                            pa->operands[1].kind == PIR_OPER_IMM_INT) {
                            int64_t off = (int64_t)eval_imm_u64(&pa->operands[1]);
                            bool fits = (off >= -256 && off <= 255) ||
                                        (off >= 0 && mb > 0u && (off % mb) == 0 && (off / mb) <= 4095);
                            if (fits) const_pa_ok_uses[pv]++;
                        }
                    }
                }
            }
        }
    }
    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        const PirBlock *blk = &fn->blocks.data[cfg->rpo[ri]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            const PirInst *ins = &fn->insts.data[iid];
            if (ins->op == PIR_OP_PTR_ADD && ins->results[0] != PIR_INVALID_ID &&
                ins->operands[0].kind == PIR_OPER_VAL && ins->operands[1].kind == PIR_OPER_IMM_INT &&
                c->val_uses[ins->results[0]] > 0u &&
                const_pa_ok_uses[ins->results[0]] == c->val_uses[ins->results[0]]) {
                c->inst_fused[iid] = true;
            }
        }
    }
    free(const_pa_ok_uses);

    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        PirBlockId bid = cfg->rpo[ri];
        const PirBlock *blk = &fn->blocks.data[bid];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            PirInstId iid = blk->insts.data[j];
            const PirInst *ins = &fn->insts.data[iid];

            /* Fuse cmp/fcmp + br for any single-use scalar cmp in the same block (SSA operands are immutable). */
            if (ins->op == PIR_OP_BR && ins->operands[0].kind == PIR_OPER_VAL) {
                PirValueId cv = ins->operands[0].u.val;
                const PirValueDef *vd = &fn->values.data[cv];
                if (vd->def_inst != PIR_INVALID_ID && vd->def_block == bid && c->val_uses[cv] == 1u) {
                    const PirInst *prev = &fn->insts.data[vd->def_inst];
                    if (prev->ty.kind == PIR_TYPE_SCALAR) {
                        MCond dummy;
                        if ((prev->op >= PIR_OP_CMP_EQ && prev->op <= PIR_OP_CMP_GE_U) ||
                            (prev->op >= PIR_OP_FCMP_OEQ && prev->op <= PIR_OP_FCMP_UGE &&
                             fcmp_single_cond(prev->op, &dummy)) ||
                            (prev->op >= PIR_OP_ADD_OVF_S && prev->op <= PIR_OP_MUL_OVF_U &&
                             cv == prev->results[1] && j > 0u && blk->insts.data[j - 1u] == vd->def_inst)) {
                            c->inst_fused[vd->def_inst] = true;
                        }
                    }
                }
            }

            /* Fuse cmp/fcmp + select (scalar csel/fcsel, or vector NEON fmin/fmax). */
            if (ins->op == PIR_OP_SELECT && ins->operands[0].kind == PIR_OPER_VAL) {
                if (ins->ty.kind == PIR_TYPE_VECTOR) {
                    PirInstId fcid = PIR_INVALID_ID;
                    MOpcode fmop = MI_NOP;
                    PirOperand fa, fb;
                    if (try_match_vec_fminmax(fn, c->val_uses, bid, ins, &fcid, &fmop, &fa, &fb)) {
                        c->inst_fused[fcid] = true;
                    }
                } else {
                    PirValueId cv = ins->operands[0].u.val;
                    const PirValueDef *vd = &fn->values.data[cv];
                    if (vd->def_inst != PIR_INVALID_ID && vd->def_block == bid &&
                        c->val_uses[cv] >= 1u && c->val_uses[cv] <= 4u &&
                        !c->inst_fused[vd->def_inst]) {
                        uint32_t sel_uses = 0u;
                        for (uint32_t sj = 0u; sj < blk->insts.len; ++sj) {
                            const PirInst *si = &fn->insts.data[blk->insts.data[sj]];
                            if (si->op == PIR_OP_SELECT && si->ty.kind == PIR_TYPE_SCALAR &&
                                si->operands[0].kind == PIR_OPER_VAL &&
                                si->operands[0].u.val == cv) {
                                sel_uses++;
                            }
                        }
                        if (sel_uses == c->val_uses[cv]) {
                            const PirInst *prev = &fn->insts.data[vd->def_inst];
                            if (prev->ty.kind == PIR_TYPE_SCALAR) {
                                MCond dummy;
                                if ((prev->op >= PIR_OP_CMP_EQ && prev->op <= PIR_OP_CMP_GE_U) ||
                                    (prev->op >= PIR_OP_FCMP_OEQ && prev->op <= PIR_OP_FCMP_UGE &&
                                     fcmp_single_cond(prev->op, &dummy))) {
                                    c->inst_fused[vd->def_inst] = true;
                                }
                            }
                        }
                    }
                }
            }

            /* Fuse (x >> r) |/^/+ (x << (W - r)) -> ARM64 ror / rorv. */
            if (ins->op == PIR_OP_OR || ins->op == PIR_OP_XOR || ins->op == PIR_OP_ADD) {
                PirInstId shr_id = PIR_INVALID_ID, shl_id = PIR_INVALID_ID;
                PirOperand rx, rn;
                bool r_is_imm = false;
                uint32_t r_imm = 0u;
                if (try_match_ror(fn, c->val_uses, bid, ins,
                                  &shr_id, &shl_id, &rx, &r_is_imm, &r_imm, &rn)) {
                    c->inst_fused[shr_id] = true;
                    c->inst_fused[shl_id] = true;
                }
            }

            /* Fuse single-use mul + add -> ARM64 madd. */
            if (ins->op == PIR_OP_ADD) {
                PirInstId mul_id = PIR_INVALID_ID;
                PirOperand m0, m1, ra;
                if (try_match_madd(fn, c->val_uses, bid, ins, &mul_id, &m0, &m1, &ra)) {
                    c->inst_fused[mul_id] = true;
                }
            }

            /* Fuse ptr_add + load/store when single-use in the same block! */
            if (ins->op == PIR_OP_LOAD || ins->op == PIR_OP_STORE) {
                const PirOperand *pop = (ins->op == PIR_OP_LOAD) ? &ins->operands[0] : &ins->operands[1];
                uint8_t mb = pir_type_mem_bytes(ins->ty);
                if (pop->kind == PIR_OPER_VAL && c->val_uses[pop->u.val] == 1u) {
                    const PirValueDef *vd = &fn->values.data[pop->u.val];
                    if (vd->def_inst != PIR_INVALID_ID && vd->def_block == bid) {
                        const PirInst *pa = &fn->insts.data[vd->def_inst];
                        if (pa->op == PIR_OP_PTR_ADD && pa->operands[0].kind == PIR_OPER_VAL) {
                            if (pa->operands[1].kind == PIR_OPER_IMM_INT) {
                                int64_t off = (int64_t)eval_imm_u64(&pa->operands[1]);
                                bool fits = (off >= -256 && off <= 255) ||
                                            (off >= 0 && mb > 0u && (off % mb) == 0 && (off / mb) <= 4095);
                                if (fits) c->inst_fused[vd->def_inst] = true;
                            } else if (pa->operands[1].kind == PIR_OPER_VAL &&
                                       pa->ty.kind == PIR_TYPE_SCALAR &&
                                       pa->ty.u.scalar == PIR_SCALAR_I32) {
                                c->inst_fused[vd->def_inst] = true;
                            } else if (pa->operands[1].kind == PIR_OPER_VAL &&
                                       pa->ty.kind == PIR_TYPE_SCALAR &&
                                       pa->ty.u.scalar == PIR_SCALAR_I64) {
                                c->inst_fused[vd->def_inst] = true;
                                PirValueId iv = pa->operands[1].u.val;
                                const PirValueDef *ivd = &fn->values.data[iv];
                                if (ivd->def_inst != PIR_INVALID_ID && ivd->def_block == bid &&
                                    c->val_uses[iv] == 1u) {
                                    const PirInst *sh = &fn->insts.data[ivd->def_inst];
                                    if (sh->op == PIR_OP_SHL && (sh->flags & PIR_FLAG_TRAP) == 0u &&
                                        sh->operands[0].kind == PIR_OPER_VAL &&
                                        sh->operands[1].kind == PIR_OPER_IMM_INT) {
                                        uint64_t amt = eval_imm_u64(&sh->operands[1]);
                                        if (amt == 0u || (amt < 64u && mb > 1u && ((uint64_t)1u << amt) == (uint64_t)mb)) {
                                            c->inst_fused[ivd->def_inst] = true;
                                            iv = sh->operands[0].u.val;
                                        }
                                    }
                                }
                                /* Also fuse single-use sext.i64(i32) -> [base, wIdx, sxtw #amt] */
                                const PirValueDef *svd = &fn->values.data[iv];
                                if (svd->def_inst != PIR_INVALID_ID && svd->def_block == bid &&
                                    c->val_uses[iv] == 1u) {
                                    const PirInst *sx = &fn->insts.data[svd->def_inst];
                                    if (sx->op == PIR_OP_SEXT && sx->operands[0].kind == PIR_OPER_VAL) {
                                        PirType raw_ty = fn->values.data[sx->operands[0].u.val].type;
                                        if (raw_ty.kind == PIR_TYPE_SCALAR &&
                                            raw_ty.u.scalar == PIR_SCALAR_I32) {
                                            c->inst_fused[svd->def_inst] = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

/* =========================================================================
 * Public Function ISel Entry Point
 * ========================================================================= */

bool pir_arm64_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                         const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);

    PirCfg cfg;
    pir_cfg_build(&cfg, fn);

    ISelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod            = mod;
    ctx.fn             = fn;
    ctx.target         = target;
    ctx.mf             = out_mf;
    ctx.diag           = diag;
    ctx.multi_ret_slot = UINT32_MAX;

    ctx.val_vreg    = (MVReg *)malloc(((size_t)fn->values.len + 1u) * sizeof(MVReg));
    ctx.pir_to_mblk = (uint32_t *)malloc(((size_t)fn->blocks.len + 1u) * sizeof(uint32_t));
    ctx.val_uses    = (uint32_t *)calloc((size_t)fn->values.len + 1u, sizeof(uint32_t));
    ctx.inst_fused  = (bool *)calloc((size_t)fn->insts.len + 1u, sizeof(bool));
    if (!ctx.val_vreg || !ctx.pir_to_mblk || !ctx.val_uses || !ctx.inst_fused) abort();

    for (uint32_t v = 0u; v < fn->values.len; ++v) {
        PirType vt = fn->values.data[v].type;
        if (vt.kind == PIR_TYPE_SCALAR && vt.u.scalar == PIR_SCALAR_VOID) {
            ctx.val_vreg[v] = MVREG_NONE;
        } else {
            ctx.val_vreg[v] = new_vreg_for_type(&ctx, vt);
        }
    }

    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        ctx.pir_to_mblk[b] = MBLOCK_NONE;
    }
    for (uint32_t ri = 0u; ri < cfg.rpo_len; ++ri) {
        PirBlockId bid = cfg.rpo[ri];
        ctx.pir_to_mblk[bid] = mfunc_new_block(out_mf, fn->blocks.data[bid].label);
    }

    analyze_fusion(&ctx, &cfg);

    /* Emit incoming parameter copies at start of entry block (MBlock 0).
     * IMPORTANT: Copy ALL incoming ABI registers (x0..x7, v0..v7, x8) FIRST before
     * emitting any slot address / zero-init instructions that could clobber x0..x7! */
    ctx.cur_mblk = 0u;
    if (fn->param_count > 0u) {
        uint32_t entry_loc = (fn->blocks.len > 0u && fn->blocks.data[0].insts.len > 0u)
                             ? fn->insts.data[fn->blocks.data[0].insts.data[0]].loc_idx : 0u;
        bool has_hidden_out = (fn->cabi_ret.kind == PIR_CABI_RET_SRET_X8 ||
                               fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG);
        uint32_t p_skip = has_hidden_out ? 1u : 0u;

        if (fn->cabi_ret.kind == PIR_CABI_RET_SRET_X8) {
            PirValueId pv0 = fn->params[0].val_id;
            if (pv0 != PIR_INVALID_ID) {
                MVReg dst0 = ctx.val_vreg[pv0];
                out_mf->vregs.data[dst0].hint_preg = A64_GP(8u);
                MInst cp = minst_make(MI_COPY, 8u, entry_loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_preg_use(A64_GP(8u));
                emit(&ctx, cp);
            }
        }

        uint32_t n_in = fn->param_count - p_skip;
        if (n_in > 0u) {
            PirType  *ptys    = (PirType *)calloc(n_in, sizeof(PirType));
            uint32_t *paligns = (uint32_t *)calloc(n_in, sizeof(uint32_t));
            ArgLoc   *plocs   = (ArgLoc *)calloc(n_in, sizeof(ArgLoc));
            if (!ptys || !paligns || !plocs) abort();
            for (uint32_t i = 0u; i < n_in; ++i) {
                ptys[i]    = fn->params[i + p_skip].type;
                paligns[i] = fn->params[i + p_skip].abi_stack_align;
            }
            uint64_t in_stack = 0u;
            uint32_t gp_used = 0u, fp_used = 0u;
            plan_call_args(target, ptys, paligns, n_in, false, n_in,
                           plocs, &in_stack, &gp_used, &fp_used);
            out_mf->va_fixed_gp          = gp_used;
            out_mf->va_fixed_fp          = fp_used;
            out_mf->va_fixed_stack_bytes = in_stack;

            /* Copy register params first, then load stack params. */
            for (uint32_t i = 0u; i < n_in; ++i) {
                PirValueId pv = fn->params[i + p_skip].val_id;
                if (pv == PIR_INVALID_ID || !plocs[i].is_reg) continue;
                MVReg dst = ctx.val_vreg[pv];
                out_mf->vregs.data[dst].hint_preg = plocs[i].preg;
                MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(ptys[i]), entry_loc);
                cp.op_count = 2u;
                cp.ops[0]   = mop_vreg_def(dst);
                cp.ops[1]   = mop_preg_use(plocs[i].preg);
                emit(&ctx, cp);
            }
            for (uint32_t i = 0u; i < n_in; ++i) {
                PirValueId pv = fn->params[i + p_skip].val_id;
                if (pv == PIR_INVALID_ID || plocs[i].is_reg) continue;
                MVReg dst = ctx.val_vreg[pv];
                MInst ld = minst_make(MI_LOAD_INARG, plocs[i].mem_bytes, entry_loc);
                ld.op_count = 2u;
                ld.ops[0]   = mop_vreg_def(dst);
                ld.ops[1]   = mop_imm(plocs[i].stack_off);
                emit(&ctx, ld);
            }
            free(ptys);
            free(paligns);
            free(plocs);
        }

        if (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG) {
            uint64_t reg_bytes = 0u;
            uint32_t reg_align = 8u;
            for (uint32_t k = 0u; k < fn->cabi_ret.reg_count; ++k) {
                uint8_t mb = pir_type_mem_bytes(fn->cabi_ret.regs[k]);
                reg_bytes += mb;
                if (mb > reg_align) reg_align = mb;
            }
            uint64_t slot_sz = fn->cabi_ret.agg_size > reg_bytes ? fn->cabi_ret.agg_size : reg_bytes;
            uint32_t slot_al = fn->cabi_ret.agg_align > reg_align ? fn->cabi_ret.agg_align : reg_align;
            ctx.multi_ret_slot = mfunc_add_slot(out_mf, slot_sz, slot_al, false);

            PirValueId pv0 = fn->params[0].val_id;
            if (pv0 != PIR_INVALID_ID) {
                MVReg dst0 = ctx.val_vreg[pv0];
                MInst fa = minst_make(MI_FRAME_ADDR, 8u, entry_loc);
                fa.op_count = 3u;
                fa.ops[0] = mop_vreg_def(dst0);
                fa.ops[1] = mop_slot(ctx.multi_ret_slot);
                fa.ops[2] = mop_imm(0);
                emit(&ctx, fa);
                if (fn->cabi_ret.agg_size < reg_bytes) {
                    MVReg z = new_gp(&ctx, 8u);
                    emit_imm_gp(&ctx, z, 0u, 8u, entry_loc);
                    for (uint64_t zoff = 0u; zoff < reg_bytes; zoff += 8u) {
                        MInst st = minst_make(MI_A64_STR, 8u, entry_loc);
                        st.op_count = 3u;
                        st.ops[0] = mop_vreg_use(z);
                        st.ops[1] = mop_vreg_use(dst0);
                        st.ops[2] = mop_imm((int64_t)zoff);
                        emit(&ctx, st);
                    }
                }
            }
        }
    }

    /* Select instructions for every reachable block in RPO order. */
    bool ok = true;
    for (uint32_t ri = 0u; ri < cfg.rpo_len && ok; ++ri) {
        PirBlockId bid = cfg.rpo[ri];
        ctx.cur_mblk = ctx.pir_to_mblk[bid];
        ctx.last_flags_cmp_id = PIR_INVALID_ID;
        const PirBlock *blk = &fn->blocks.data[bid];
        for (uint32_t j = 0u; j < blk->insts.len && ok; ++j) {
            ok = isel_inst(&ctx, blk->insts.data[j], bid);
        }
    }

    free(ctx.val_vreg);
    free(ctx.pir_to_mblk);
    free(ctx.val_uses);
    free(ctx.inst_fused);
    pir_cfg_destroy(&cfg);
    return ok;
}
