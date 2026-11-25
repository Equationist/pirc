/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <limits.h>
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
            case PIR_SCALAR_VOID:
            case PIR_SCALAR_I128:
            case PIR_SCALAR_VALIST:
                return 8u;
        }
    }
    return 8u;
}

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

static uint64_t eval_imm_u64(const PirOperand *op) {
    uint64_t v = op->u.imm_int.lo;
    if (op->u.imm_int.is_negative) {
        v = (uint64_t)(-(int64_t)v);
    }
    return v;
}

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool fits_simm32(uint64_t val, uint8_t width) {
    if (width <= 4u) return true;
    int64_t sv = (int64_t)val;
    return sv >= INT32_MIN && sv <= INT32_MAX;
}

static int64_t sign_ext_imm(uint64_t val, uint8_t width) {
    if (width <= 4u) return (int64_t)(int32_t)(uint32_t)val;
    return (int64_t)val;
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
    uint32_t            *val_uses;
    bool                *inst_fused;
    PirInstId            last_flags_cmp_id;
    uint32_t             cur_mblk;
    uint32_t             multi_ret_slot;
    MVReg                sret_ptr_vreg;
    uint32_t             vec_tmp_slot[3]; /* Reusable 16B stack scratch slots for baseline SSE2 lanes */
} X64ISelCtx;

static MBlock *cur_blk(X64ISelCtx *c) {
    return &c->mf->blocks.data[c->cur_mblk];
}

static void emit(X64ISelCtx *c, MInst ins) {
    mblock_push(cur_blk(c), ins);
}

static MVReg new_gp(X64ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_GP, bytes <= 4u ? 4u : 8u);
}

static MVReg new_fp(X64ISelCtx *c, uint8_t bytes) {
    return mfunc_new_vreg(c->mf, MREG_CLASS_FP, bytes);
}

static MVReg new_vreg_for_type(X64ISelCtx *c, PirType ty) {
    return mfunc_new_vreg(c->mf, pir_type_rclass(ty), pir_type_vreg_bytes(ty));
}

static uint32_t get_vec_tmp_slot(X64ISelCtx *c, uint32_t idx) {
    if (c->vec_tmp_slot[idx] == UINT32_MAX) {
        c->vec_tmp_slot[idx] = mfunc_add_slot(c->mf, 16u, 16u, false);
    }
    return c->vec_tmp_slot[idx];
}

static void emit_copy(X64ISelCtx *c, MVReg dst, MVReg src, uint8_t w, uint32_t loc) {
    MInst cp = minst_make(MI_COPY, w, loc);
    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_vreg_use(src);
    emit(c, cp);
}

static void emit_imm_gp(X64ISelCtx *c, MVReg dst, uint64_t val, uint8_t width, uint32_t loc) {
    uint8_t w = width <= 4u ? 4u : 8u;
    MInst m = minst_make(MI_X64_MOV_RI, w, loc);
    m.op_count = 2u;
    m.ops[0]   = mop_vreg_def(dst);
    m.ops[1]   = mop_imm((int64_t)(w == 4u ? (val & 0xFFFFFFFFu) : val));
    emit(c, m);
}

static void emit_imm_fp(X64ISelCtx *c, MVReg dst, uint64_t bits, uint8_t width, uint32_t loc) {
    MVReg gp = new_gp(c, width);
    emit_imm_gp(c, gp, bits, width, loc);
    MInst mv = minst_make(MI_X64_MOV_G2X, width, loc);
    mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(gp);
    emit(c, mv);
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

static void emit_sym_addr(X64ISelCtx *c, MVReg dst, const char *name, int64_t offset, uint32_t loc) {
    bool is_func = false, is_tls = false, use_got = false;
    lookup_symbol_meta(c->mod, name, &is_func, &is_tls, &use_got);

    MVReg base = (offset != 0) ? new_gp(c, 8u) : dst;
    if (is_tls) {
        if (c->target->os == PIR_OS_MACOS) {
            /* Darwin x86-64 TLV thunk call: mov rdi, [rip+_sym@TLVP]; call [rdi] -> rax. */
            c->mf->has_calls = true;
            MInst t = minst_make(MI_X64_TLS_ADDR, 8u, loc);
            t.is_call  = true;
            t.op_count = 2u;
            t.ops[0]   = mop_preg_def(X64_RAX);
            t.ops[1]   = mop_sym(name, 0, false, true, use_got);
            emit(c, t);
            MInst cp = minst_make(MI_COPY, 8u, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(base); cp.ops[1] = mop_preg_use(X64_RAX);
            emit(c, cp);
        } else {
            MInst t = minst_make(MI_X64_TLS_ADDR, 8u, loc);
            t.op_count = 2u;
            t.ops[0]   = mop_vreg_def(base);
            t.ops[1]   = mop_sym(name, 0, false, true, false);
            emit(c, t);
        }
    } else {
        MInst a = minst_make(MI_X64_SYM_ADDR, 8u, loc);
        a.op_count = 2u;
        a.ops[0]   = mop_vreg_def(base);
        a.ops[1]   = mop_sym(name, 0, is_func, false, use_got);
        emit(c, a);
    }

    if (offset != 0) {
        if (offset >= INT32_MIN && offset <= INT32_MAX) {
            MInst lea = minst_make(MI_X64_LEA, 8u, loc);
            lea.op_count = 3u;
            lea.ops[0] = mop_vreg_def(dst);
            lea.ops[1] = mop_vreg_use(base);
            lea.ops[2] = mop_imm(offset);
            emit(c, lea);
        } else {
            MVReg off_r = new_gp(c, 8u);
            emit_imm_gp(c, off_r, (uint64_t)offset, 8u, loc);
            emit_copy(c, dst, base, 8u, loc);
            MInst ad = minst_make(MI_X64_ADD_RR, 8u, loc);
            ad.op_count = 2u; ad.ops[0] = mop_vreg_defuse(dst); ad.ops[1] = mop_vreg_use(off_r);
            emit(c, ad);
        }
    }
}

static MVReg mat_op(X64ISelCtx *c, const PirOperand *op, uint32_t loc) {
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
            MVReg fp = new_fp(c, w);
            emit_imm_fp(c, fp, op->u.imm_float.bits, w, loc);
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

static MVReg normalize_int(X64ISelCtx *c, MVReg src, PirType ty, bool is_signed, uint32_t loc) {
    if (ty.kind != PIR_TYPE_SCALAR) return src;
    if (ty.u.scalar == PIR_SCALAR_I8 || ty.u.scalar == PIR_SCALAR_I16) {
        MVReg r = new_gp(c, 4u);
        MInst e = minst_make(is_signed ? MI_X64_MOVSX : MI_X64_MOVZX, 4u, loc);
        e.aux_width = (ty.u.scalar == PIR_SCALAR_I8) ? 1u : 2u;
        e.op_count = 2u; e.ops[0] = mop_vreg_def(r); e.ops[1] = mop_vreg_use(src);
        emit(c, e);
        return r;
    }
    return src;
}

static void emit_cond_trap(X64ISelCtx *c, MCond trap_cond, uint32_t loc) {
    uint32_t trap_blk = mfunc_new_block(c->mf, "trap");
    uint32_t ok_blk   = mfunc_new_block(c->mf, "ok");

    MInst jc = minst_make(MI_X64_JCC, 4u, loc);
    jc.op_count = 2u; jc.ops[0] = mop_cond(trap_cond); jc.ops[1] = mop_block(trap_blk);
    emit(c, jc);

    MInst j_ok = minst_make(MI_X64_JMP, 4u, loc);
    j_ok.op_count = 1u; j_ok.ops[0] = mop_block(ok_blk);
    emit(c, j_ok);

    mfunc_add_edge(c->mf, c->cur_mblk, trap_blk);
    mfunc_add_edge(c->mf, c->cur_mblk, ok_blk);

    MInst ud2 = minst_make(MI_X64_UD2, 4u, loc);
    mblock_push(&c->mf->blocks.data[trap_blk], ud2);

    c->cur_mblk = ok_blk;
}

/* =========================================================================
 * Edge Parallel Copy Helper
 * ========================================================================= */

static uint32_t build_edge_pcopy(X64ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
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
            entries[cnt].src_is_imm = false;
            entries[cnt].src        = mat_op(c, &bt->args[i], loc);
        }
        cnt++;
    }
    uint32_t id = mfunc_add_pcopy(c->mf, entries, cnt);
    free(entries);
    return id;
}

static uint32_t prepare_branch_edge(X64ISelCtx *c, const PirBranchTarget *bt, uint32_t loc) {
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
    MInst jmp = minst_make(MI_X64_JMP, 4u, loc);
    jmp.op_count = 1u; jmp.ops[0] = mop_block(dst_mblk);
    emit(c, jmp);
    mfunc_add_edge(c->mf, tramp, dst_mblk);
    c->cur_mblk = saved_blk;
    return tramp;
}

/* =========================================================================
 * Comparison Helpers (Int & Float + Fusion)
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

/* x86-64 ucomisd/ucomiss single-cc predicates:
 *   NaN: ZF=1,PF=1,CF=1;  ==: ZF=1,PF=0,CF=0;  <: ZF=0,PF=0,CF=1;  >: ZF=0,PF=0,CF=0.
 */
static bool x64_fcmp_single(PirOpcode op, MCond *out_cond, bool *out_swap) {
    *out_swap = false;
    if (op == PIR_OP_FCMP_OGT) { *out_cond = MCOND_HI; return true; }
    if (op == PIR_OP_FCMP_OGE) { *out_cond = MCOND_CS; return true; }
    if (op == PIR_OP_FCMP_OLT) { *out_cond = MCOND_HI; *out_swap = true; return true; }
    if (op == PIR_OP_FCMP_OLE) { *out_cond = MCOND_CS; *out_swap = true; return true; }
    if (op == PIR_OP_FCMP_ULT) { *out_cond = MCOND_CC; return true; }
    if (op == PIR_OP_FCMP_ULE) { *out_cond = MCOND_LS; return true; }
    if (op == PIR_OP_FCMP_UGT) { *out_cond = MCOND_CC; *out_swap = true; return true; }
    if (op == PIR_OP_FCMP_UGE) { *out_cond = MCOND_LS; *out_swap = true; return true; }
    if (op == PIR_OP_FCMP_ONE) { *out_cond = MCOND_NE; return true; }
    if (op == PIR_OP_FCMP_UEQ) { *out_cond = MCOND_EQ; return true; }
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
        return true;
    }
    if (sc == PIR_SCALAR_I8 && d->ty.kind == PIR_TYPE_SCALAR &&
        ((d->op >= PIR_OP_CMP_EQ && d->op <= PIR_OP_CMP_GE_U) ||
         (d->op >= PIR_OP_FCMP_OEQ && d->op <= PIR_OP_FCMP_UGE))) {
        return true;
    }
    return false;
}

static void emit_scalar_int_cmp_flags(X64ISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_signed = int_cmp_is_signed(inst->op);
    uint8_t w = pir_type_vreg_bytes(inst->ty);
    MVReg lhs_raw = mat_op(c, &inst->operands[0], loc);
    MVReg lhs = (!is_signed && is_already_zext_subword(c->fn, &inst->operands[0], inst->ty))
                ? lhs_raw
                : normalize_int(c, lhs_raw, inst->ty, is_signed, loc);

    if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
        uint64_t raw = eval_imm_u64(&inst->operands[1]);
        if (w == 8u || (inst->ty.kind == PIR_TYPE_SCALAR && inst->ty.u.scalar == PIR_SCALAR_I32)) {
            if (raw == 0u && (inst->op == PIR_OP_CMP_EQ || inst->op == PIR_OP_CMP_NE ||
                              inst->op == PIR_OP_CMP_LT_S || inst->op == PIR_OP_CMP_GE_S ||
                              inst->op == PIR_OP_CMP_GT_S || inst->op == PIR_OP_CMP_LE_S)) {
                MInst t = minst_make(MI_X64_TEST_RR, w, loc);
                t.op_count = 2u; t.ops[0] = mop_vreg_use(lhs); t.ops[1] = mop_vreg_use(lhs);
                emit(c, t);
                return;
            }
            if (fits_simm32(raw, w)) {
                MInst cmp = minst_make(MI_X64_CMP_RI, w, loc);
                cmp.op_count = 2u;
                cmp.ops[0]   = mop_vreg_use(lhs);
                cmp.ops[1]   = mop_imm(sign_ext_imm(raw, w));
                emit(c, cmp);
                return;
            }
        } else if (inst->ty.kind == PIR_TYPE_SCALAR &&
                   (inst->ty.u.scalar == PIR_SCALAR_I8 || inst->ty.u.scalar == PIR_SCALAR_I16)) {
            int64_t norm = (inst->ty.u.scalar == PIR_SCALAR_I8)
                           ? (is_signed ? (int64_t)(int8_t)raw : (int64_t)(uint8_t)raw)
                           : (is_signed ? (int64_t)(int16_t)raw : (int64_t)(uint16_t)raw);
            MInst cmp = minst_make(MI_X64_CMP_RI, 4u, loc);
            cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(lhs); cmp.ops[1] = mop_imm(norm);
            emit(c, cmp);
            return;
        }
    }
    MVReg rhs_raw = mat_op(c, &inst->operands[1], loc);
    MVReg rhs = (!is_signed && is_already_zext_subword(c->fn, &inst->operands[1], inst->ty))
                ? rhs_raw
                : normalize_int(c, rhs_raw, inst->ty, is_signed, loc);
    MInst cmp = minst_make(MI_X64_CMP_RR, w, loc);
    cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(lhs); cmp.ops[1] = mop_vreg_use(rhs);
    emit(c, cmp);
}

/* =========================================================================
 * Vector (Baseline SSE2) Helpers
 * ========================================================================= */

static void emit_vec_store_slot(X64ISelCtx *c, MVReg v, uint32_t slot_id, uint8_t vb, uint32_t loc) {
    MInst st = minst_make(MI_STORE_SLOT, vb, loc);
    st.op_count = 2u; st.ops[0] = mop_vreg_use(v); st.ops[1] = mop_slot(slot_id);
    emit(c, st);
}

static void emit_vec_load_slot(X64ISelCtx *c, MVReg dst, uint32_t slot_id, uint8_t vb, uint32_t loc) {
    MInst ld = minst_make(MI_LOAD_SLOT, vb, loc);
    ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_slot(slot_id);
    emit(c, ld);
}

static MVReg emit_slot_addr(X64ISelCtx *c, uint32_t slot_id, int64_t off, uint32_t loc) {
    MVReg p = new_gp(c, 8u);
    MInst fa = minst_make(MI_FRAME_ADDR, 8u, loc);
    fa.op_count = 3u;
    fa.ops[0] = mop_vreg_def(p); fa.ops[1] = mop_slot(slot_id); fa.ops[2] = mop_imm(off);
    emit(c, fa);
    return p;
}

static void emit_vec_lane_extract(X64ISelCtx *c, MVReg dst, MVReg vec, PirVecType vt,
                                  uint32_t lane, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    uint8_t eb = pir_type_mem_bytes(pir_type_scalar(el));
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
    if (lane == 0u) {
        if (pir_scalar_is_float(el)) {
            emit_copy(c, dst, vec, eb, loc);
            return;
        }
        if (eb == 4u || eb == 8u) {
            MInst mv = minst_make(MI_X64_MOV_X2G, eb, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(vec);
            emit(c, mv);
            return;
        }
    }
    uint32_t sl = get_vec_tmp_slot(c, 0u);
    emit_vec_store_slot(c, vec, sl, vb, loc);
    MVReg base = emit_slot_addr(c, sl, (int64_t)(lane * eb), loc);
    MInst ld = minst_make(pir_scalar_is_float(el) ? MI_X64_MOVX_RM : MI_X64_MOV_RM, eb, loc);
    ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_imm(0);
    emit(c, ld);
}

static void emit_vec_lane_insert(X64ISelCtx *c, MVReg dst, MVReg base_vec, MVReg scalar,
                                 PirVecType vt, uint32_t lane, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    uint8_t eb = pir_type_mem_bytes(pir_type_scalar(el));
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
    uint32_t sl = get_vec_tmp_slot(c, 0u);
    emit_vec_store_slot(c, base_vec, sl, vb, loc);
    MVReg base = emit_slot_addr(c, sl, (int64_t)(lane * eb), loc);
    MInst st = minst_make(pir_scalar_is_float(el) ? MI_X64_MOVX_MR : MI_X64_MOV_MR, eb, loc);
    st.op_count = 3u; st.ops[0] = mop_vreg_use(scalar); st.ops[1] = mop_vreg_use(base); st.ops[2] = mop_imm(0);
    emit(c, st);
    emit_vec_load_slot(c, dst, sl, vb, loc);
}

static void emit_vec_splat(X64ISelCtx *c, MVReg dst, MVReg scalar, PirVecType vt, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    uint8_t eb = pir_type_mem_bytes(pir_type_scalar(el));
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
    if (eb == 4u) {
        MVReg v0 = scalar;
        if (!pir_scalar_is_float(el)) {
            v0 = new_fp(c, vb);
            MInst m = minst_make(MI_X64_MOV_G2X, 4u, loc);
            m.op_count = 2u; m.ops[0] = mop_vreg_def(v0); m.ops[1] = mop_vreg_use(scalar);
            emit(c, m);
        }
        MInst sh = minst_make(MI_X64_PSHUFD, vb, loc);
        sh.aux_width = 0x00u;
        sh.op_count = 2u; sh.ops[0] = mop_vreg_def(dst); sh.ops[1] = mop_vreg_use(v0);
        emit(c, sh);
        return;
    }
    if (eb == 8u) {
        MVReg v0 = scalar;
        if (!pir_scalar_is_float(el)) {
            v0 = new_fp(c, vb);
            MInst m = minst_make(MI_X64_MOV_G2X, 8u, loc);
            m.op_count = 2u; m.ops[0] = mop_vreg_def(v0); m.ops[1] = mop_vreg_use(scalar);
            emit(c, m);
        }
        emit_copy(c, dst, v0, vb, loc);
        MInst u = minst_make(MI_X64_PUNPCKLQDQ, vb, loc);
        u.op_count = 2u; u.ops[0] = mop_vreg_defuse(dst); u.ops[1] = mop_vreg_use(dst);
        emit(c, u);
        return;
    }
    /* i8 or i16 splat via movd + punpcklbw/punpcklwd + pshufd */
    MVReg t = new_fp(c, vb);
    MInst m = minst_make(MI_X64_MOV_G2X, 4u, loc);
    m.op_count = 2u; m.ops[0] = mop_vreg_def(t); m.ops[1] = mop_vreg_use(scalar);
    emit(c, m);
    if (eb == 1u) {
        MInst u1 = minst_make(MI_X64_PUNPCKLBW, vb, loc);
        u1.op_count = 2u; u1.ops[0] = mop_vreg_defuse(t); u1.ops[1] = mop_vreg_use(t);
        emit(c, u1);
    }
    MInst u2 = minst_make(MI_X64_PUNPCKLWD, vb, loc);
    u2.op_count = 2u; u2.ops[0] = mop_vreg_defuse(t); u2.ops[1] = mop_vreg_use(t);
    emit(c, u2);
    MInst sh = minst_make(MI_X64_PSHUFD, vb, loc);
    sh.aux_width = 0x00u;
    sh.op_count = 2u; sh.ops[0] = mop_vreg_def(dst); sh.ops[1] = mop_vreg_use(t);
    emit(c, sh);
}

static void emit_vec_all_ones(X64ISelCtx *c, MVReg dst, uint8_t vb, uint32_t loc) {
    MInst cm = minst_make(MI_X64_PCMPEQD, vb, loc);
    cm.op_count = 2u; cm.ops[0] = mop_vreg_def(dst); cm.ops[1] = mop_vreg_use(dst);
    emit(c, cm);
}

static void emit_vec_select(X64ISelCtx *c, MVReg dst, MVReg mask, MVReg tv, MVReg fv,
                            uint8_t vb, uint32_t loc) {
    MVReg t1 = new_fp(c, vb);
    MVReg t2 = new_fp(c, vb);
    emit_copy(c, t1, mask, vb, loc);
    MInst a1 = minst_make(MI_X64_PAND, vb, loc);
    a1.op_count = 2u; a1.ops[0] = mop_vreg_defuse(t1); a1.ops[1] = mop_vreg_use(tv);
    emit(c, a1);

    emit_copy(c, t2, mask, vb, loc);
    MInst a2 = minst_make(MI_X64_PANDN, vb, loc);
    a2.op_count = 2u; a2.ops[0] = mop_vreg_defuse(t2); a2.ops[1] = mop_vreg_use(fv);
    emit(c, a2);

    emit_copy(c, dst, t1, vb, loc);
    MInst o = minst_make(MI_X64_POR, vb, loc);
    o.op_count = 2u; o.ops[0] = mop_vreg_defuse(dst); o.ops[1] = mop_vreg_use(t2);
    emit(c, o);
}

static void emit_vec_cmp(X64ISelCtx *c, PirOpcode op, MVReg dst, MVReg a, MVReg b,
                         PirVecType vt, uint32_t loc) {
    PirScalarType el = pir_vec_elem_scalar(vt);
    uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);

    /* Floating-point vector comparisons via baseline SSE2 cmpps / cmppd! */
    if (pir_scalar_is_float(el)) {
        MOpcode cmop = (el == PIR_SCALAR_F32) ? MI_X64_CMPPS : MI_X64_CMPPD;
        /* SSE2 cmpps/cmppd imm8: 0=eq(OEQ), 1=lt(OLT), 2=le(OLE), 3=unord, 4=neq(UNE), 5=nlt(UGE), 6=nle(UGT), 7=ord */
        bool swap = false;
        uint8_t pred = 0u;
        bool need_and = false, need_or = false;
        uint8_t p1 = 0u, p2 = 0u;
        if      (op == PIR_OP_FCMP_OEQ) { pred = 0u; }
        else if (op == PIR_OP_FCMP_OLT) { pred = 1u; }
        else if (op == PIR_OP_FCMP_OLE) { pred = 2u; }
        else if (op == PIR_OP_FCMP_OGT) { pred = 1u; swap = true; }
        else if (op == PIR_OP_FCMP_OGE) { pred = 2u; swap = true; }
        else if (op == PIR_OP_FCMP_UNE) { pred = 4u; }
        else if (op == PIR_OP_FCMP_UGE) { pred = 5u; }
        else if (op == PIR_OP_FCMP_UGT) { pred = 6u; }
        else if (op == PIR_OP_FCMP_ULE) { pred = 5u; swap = true; }
        else if (op == PIR_OP_FCMP_ULT) { pred = 6u; swap = true; }
        else if (op == PIR_OP_FCMP_ONE) { need_and = true; p1 = 4u; p2 = 7u; }
        else                            { need_or  = true; p1 = 0u; p2 = 3u; } /* UEQ */

        if (need_and || need_or) {
            MVReg t1 = new_fp(c, vb), t2 = new_fp(c, vb);
            emit_copy(c, t1, a, vb, loc);
            MInst c1 = minst_make(cmop, vb, loc);
            c1.aux_width = p1; c1.op_count = 2u;
            c1.ops[0] = mop_vreg_defuse(t1); c1.ops[1] = mop_vreg_use(b);
            emit(c, c1);
            emit_copy(c, t2, a, vb, loc);
            MInst c2 = minst_make(cmop, vb, loc);
            c2.aux_width = p2; c2.op_count = 2u;
            c2.ops[0] = mop_vreg_defuse(t2); c2.ops[1] = mop_vreg_use(b);
            emit(c, c2);
            emit_copy(c, dst, t1, vb, loc);
            MInst comb = minst_make(need_and ? MI_X64_PAND : MI_X64_POR, vb, loc);
            comb.op_count = 2u; comb.ops[0] = mop_vreg_defuse(dst); comb.ops[1] = mop_vreg_use(t2);
            emit(c, comb);
            return;
        }
        emit_copy(c, dst, swap ? b : a, vb, loc);
        MInst cm = minst_make(cmop, vb, loc);
        cm.aux_width = pred; cm.op_count = 2u;
        cm.ops[0] = mop_vreg_defuse(dst); cm.ops[1] = mop_vreg_use(swap ? a : b);
        emit(c, cm);
        return;
    }

    /* Native SSE2 integer comparisons for 8/16/32-bit eq, ne, signed lt/le/gt/ge */
    if (el != PIR_SCALAR_I64 &&
        (op == PIR_OP_CMP_EQ || op == PIR_OP_CMP_NE ||
         op == PIR_OP_CMP_GT_S || op == PIR_OP_CMP_LT_S ||
         op == PIR_OP_CMP_GE_S || op == PIR_OP_CMP_LE_S)) {
        MOpcode eq_op = (el == PIR_SCALAR_I8) ? MI_X64_PCMPEQB
                      : (el == PIR_SCALAR_I16) ? MI_X64_PCMPEQW : MI_X64_PCMPEQD;
        MOpcode gt_op = (el == PIR_SCALAR_I8) ? MI_X64_PCMPGTB
                      : (el == PIR_SCALAR_I16) ? MI_X64_PCMPGTW : MI_X64_PCMPGTD;
        bool swap = (op == PIR_OP_CMP_LT_S || op == PIR_OP_CMP_GE_S);
        bool inv  = (op == PIR_OP_CMP_NE || op == PIR_OP_CMP_GE_S || op == PIR_OP_CMP_LE_S);
        MOpcode mop = (op == PIR_OP_CMP_EQ || op == PIR_OP_CMP_NE) ? eq_op : gt_op;
        emit_copy(c, dst, swap ? b : a, vb, loc);
        MInst cm = minst_make(mop, vb, loc);
        cm.op_count = 2u; cm.ops[0] = mop_vreg_defuse(dst); cm.ops[1] = mop_vreg_use(swap ? a : b);
        emit(c, cm);
        if (inv) {
            MVReg ones = new_fp(c, vb);
            emit_vec_all_ones(c, ones, vb, loc);
            MInst x = minst_make(MI_X64_PXOR, vb, loc);
            x.op_count = 2u; x.ops[0] = mop_vreg_defuse(dst); x.ops[1] = mop_vreg_use(ones);
            emit(c, x);
        }
        return;
    }

    /* Generic lane-wise comparison (v2i64 or unsigned integer vectors) */
    uint32_t lanes = pir_vec_lanes(vt);
    uint8_t eb = pir_type_mem_bytes(pir_type_scalar(el));
    uint8_t ew = (eb == 8u) ? 8u : 4u;
    bool is_signed = int_cmp_is_signed(op);
    uint32_t sl_a = get_vec_tmp_slot(c, 0u);
    uint32_t sl_b = get_vec_tmp_slot(c, 1u);
    uint32_t sl_d = get_vec_tmp_slot(c, 2u);
    emit_vec_store_slot(c, a, sl_a, vb, loc);
    emit_vec_store_slot(c, b, sl_b, vb, loc);
    MVReg ba = emit_slot_addr(c, sl_a, 0, loc);
    MVReg bb = emit_slot_addr(c, sl_b, 0, loc);
    MVReg bd = emit_slot_addr(c, sl_d, 0, loc);
    for (uint32_t l = 0u; l < lanes; ++l) {
        int64_t off = (int64_t)(l * eb);
        MVReg la = new_gp(c, ew), lb = new_gp(c, ew);
        MInst lda = minst_make(MI_X64_MOV_RM, eb, loc);
        lda.is_signed = is_signed; lda.op_count = 3u;
        lda.ops[0] = mop_vreg_def(la); lda.ops[1] = mop_vreg_use(ba); lda.ops[2] = mop_imm(off);
        emit(c, lda);
        MInst ldb = minst_make(MI_X64_MOV_RM, eb, loc);
        ldb.is_signed = is_signed; ldb.op_count = 3u;
        ldb.ops[0] = mop_vreg_def(lb); ldb.ops[1] = mop_vreg_use(bb); ldb.ops[2] = mop_imm(off);
        emit(c, ldb);

        MInst cmp = minst_make(MI_X64_CMP_RR, ew, loc);
        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(la); cmp.ops[1] = mop_vreg_use(lb);
        emit(c, cmp);

        MVReg msk = new_gp(c, ew);
        MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
        sc.op_count = 2u; sc.ops[0] = mop_vreg_def(msk); sc.ops[1] = mop_cond(int_cmp_to_cond(op));
        emit(c, sc);
        /* Convert 0/1 to 0 / all-ones via neg! */
        MInst ng = minst_make(MI_X64_NEG, ew, loc);
        ng.op_count = 1u; ng.ops[0] = mop_vreg_defuse(msk);
        emit(c, ng);

        MInst st = minst_make(MI_X64_MOV_MR, eb, loc);
        st.op_count = 3u; st.ops[0] = mop_vreg_use(msk); st.ops[1] = mop_vreg_use(bd); st.ops[2] = mop_imm(off);
        emit(c, st);
    }
    emit_vec_load_slot(c, dst, sl_d, vb, loc);
}

/* =========================================================================
 * Scalar Arithmetic, Div/Rem, Overflow, Bitops, Float Conversions
 * ========================================================================= */

static void emit_scalar_div_rem(X64ISelCtx *c, const PirInst *inst, MVReg dst, uint32_t loc) {
    bool is_signed = (inst->op == PIR_OP_DIV_S || inst->op == PIR_OP_REM_S);
    bool is_rem    = (inst->op == PIR_OP_REM_S || inst->op == PIR_OP_REM_U);
    uint8_t w      = pir_type_vreg_bytes(inst->ty);
    MVReg a = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, is_signed, loc);
    MVReg b = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, is_signed, loc);

    if ((inst->flags & PIR_FLAG_TRAP) != 0u) {
        MInst tz = minst_make(MI_X64_TEST_RR, w, loc);
        tz.op_count = 2u; tz.ops[0] = mop_vreg_use(b); tz.ops[1] = mop_vreg_use(b);
        emit(c, tz);
        emit_cond_trap(c, MCOND_EQ, loc);

        if (is_signed) {
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint64_t int_min = (bits == 64u) ? ((uint64_t)1u << 63u)
                                             : (uint64_t)(int64_t)(int32_t)(-(int32_t)(1u << (bits - 1u)));
            MInst cm1 = minst_make(MI_X64_CMP_RI, w, loc);
            cm1.op_count = 2u; cm1.ops[0] = mop_vreg_use(b); cm1.ops[1] = mop_imm(-1);
            emit(c, cm1);
            MVReg is_m1 = new_gp(c, 4u);
            MInst s1 = minst_make(MI_X64_SETCC, 4u, loc);
            s1.op_count = 2u; s1.ops[0] = mop_vreg_def(is_m1); s1.ops[1] = mop_cond(MCOND_EQ);
            emit(c, s1);

            MVReg imin = new_gp(c, w);
            emit_imm_gp(c, imin, int_min, w, loc);
            MInst cmin = minst_make(MI_X64_CMP_RR, w, loc);
            cmin.op_count = 2u; cmin.ops[0] = mop_vreg_use(a); cmin.ops[1] = mop_vreg_use(imin);
            emit(c, cmin);
            MVReg is_min = new_gp(c, 4u);
            MInst s2 = minst_make(MI_X64_SETCC, 4u, loc);
            s2.op_count = 2u; s2.ops[0] = mop_vreg_def(is_min); s2.ops[1] = mop_cond(MCOND_EQ);
            emit(c, s2);

            MInst an = minst_make(MI_X64_TEST_RR, 4u, loc);
            an.op_count = 2u; an.ops[0] = mop_vreg_use(is_m1); an.ops[1] = mop_vreg_use(is_min);
            emit(c, an);
            emit_cond_trap(c, MCOND_NE, loc);
        }
    }

    uint32_t done_blk = MBLOCK_NONE;
    if (is_signed && (inst->flags & (PIR_FLAG_TRAP | PIR_FLAG_UNSAFE)) == 0u) {
        /* x86-64 idiv raises SIGFPE on INT_MIN / -1, whereas PIR [wrap]/default
         * defines INT_MIN / -1 == INT_MIN and INT_MIN % -1 == 0.
         * Fast 1-branch guard: when divisor == -1, quotient is -a (wraps INT_MIN) and rem is 0! */
        uint32_t neg1_blk = mfunc_new_block(c->mf, "div_m1");
        uint32_t div_blk  = mfunc_new_block(c->mf, "div_do");
        done_blk          = mfunc_new_block(c->mf, "div_dn");

        MInst cm1 = minst_make(MI_X64_CMP_RI, w, loc);
        cm1.op_count = 2u; cm1.ops[0] = mop_vreg_use(b); cm1.ops[1] = mop_imm(-1);
        emit(c, cm1);
        MInst je = minst_make(MI_X64_JCC, 4u, loc);
        je.op_count = 2u; je.ops[0] = mop_cond(MCOND_EQ); je.ops[1] = mop_block(neg1_blk);
        emit(c, je);
        MInst jd = minst_make(MI_X64_JMP, 4u, loc);
        jd.op_count = 1u; jd.ops[0] = mop_block(div_blk);
        emit(c, jd);
        mfunc_add_edge(c->mf, c->cur_mblk, neg1_blk);
        mfunc_add_edge(c->mf, c->cur_mblk, div_blk);

        c->cur_mblk = neg1_blk;
        if (is_rem) {
            emit_imm_gp(c, dst, 0u, w, loc);
        } else {
            emit_copy(c, dst, a, w, loc);
            MInst ng = minst_make(MI_X64_NEG, w, loc);
            ng.op_count = 1u; ng.ops[0] = mop_vreg_defuse(dst);
            emit(c, ng);
        }
        MInst j1 = minst_make(MI_X64_JMP, 4u, loc);
        j1.op_count = 1u; j1.ops[0] = mop_block(done_blk);
        emit(c, j1);
        mfunc_add_edge(c->mf, neg1_blk, done_blk);

        c->cur_mblk = div_blk;
    }

    /* Copy a -> rax, sign/zero extend into rdx, then idiv/div b. */
    MInst ma = minst_make(MI_COPY, w, loc);
    ma.op_count = 2u; ma.ops[0] = mop_preg_def(X64_RAX); ma.ops[1] = mop_vreg_use(a);
    emit(c, ma);

    if (is_signed) {
        MInst cqo = minst_make(MI_X64_CQO, w, loc);
        cqo.op_count = 2u; cqo.ops[0] = mop_preg_def(X64_RDX); cqo.ops[1] = mop_preg_use(X64_RAX);
        emit(c, cqo);
    } else {
        MInst zd = minst_make(MI_X64_MOV_RI, 4u, loc);
        zd.op_count = 2u; zd.ops[0] = mop_preg_def(X64_RDX); zd.ops[1] = mop_imm(0);
        emit(c, zd);
    }

    MInst dv = minst_make(is_signed ? MI_X64_IDIV : MI_X64_DIV, w, loc);
    dv.op_count = 3u;
    dv.ops[0]   = mop_vreg_use(b);
    dv.ops[1]   = mop_preg_defuse(X64_RAX);
    dv.ops[2]   = mop_preg_defuse(X64_RDX);
    emit(c, dv);

    MInst out = minst_make(MI_COPY, w, loc);
    out.op_count = 2u;
    out.ops[0]   = mop_vreg_def(dst);
    out.ops[1]   = mop_preg_use(is_rem ? X64_RDX : X64_RAX);
    emit(c, out);

    if (done_blk != MBLOCK_NONE) {
        MInst j2 = minst_make(MI_X64_JMP, 4u, loc);
        j2.op_count = 1u; j2.ops[0] = mop_block(done_blk);
        emit(c, j2);
        mfunc_add_edge(c->mf, c->cur_mblk, done_blk);
        c->cur_mblk = done_blk;
    }
}

static void emit_scalar_ovf(X64ISelCtx *c, PirOpcode op, PirType ty,
                            MVReg a_raw, MVReg b_raw,
                            MVReg dst_val, MVReg dst_ovf, uint32_t loc) {
    bool is_signed = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_SUB_OVF_S || op == PIR_OP_MUL_OVF_S);
    uint32_t bits  = pir_scalar_bits(ty.u.scalar, 8u);
    uint8_t w      = pir_type_vreg_bytes(ty);

    if (bits < 32u) {
        MVReg a = normalize_int(c, a_raw, ty, is_signed, loc);
        MVReg b = normalize_int(c, b_raw, ty, is_signed, loc);
        emit_copy(c, dst_val, a, 4u, loc);
        MOpcode alu = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U) ? MI_X64_ADD_RR
                    : (op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) ? MI_X64_SUB_RR
                                                                         : MI_X64_IMUL_RR;
        MInst m = minst_make(alu, 4u, loc);
        m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst_val); m.ops[1] = mop_vreg_use(b);
        emit(c, m);
        MVReg trunc_back = normalize_int(c, dst_val, ty, is_signed, loc);
        MInst cmp = minst_make(MI_X64_CMP_RR, 4u, loc);
        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(dst_val); cmp.ops[1] = mop_vreg_use(trunc_back);
        emit(c, cmp);
        MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
        sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst_ovf); sc.ops[1] = mop_cond(MCOND_NE);
        emit(c, sc);
        return;
    }

    if (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U ||
        op == PIR_OP_SUB_OVF_S || op == PIR_OP_SUB_OVF_U) {
        bool is_add = (op == PIR_OP_ADD_OVF_S || op == PIR_OP_ADD_OVF_U);
        emit_copy(c, dst_val, a_raw, w, loc);
        MInst m = minst_make(is_add ? MI_X64_ADD_RR : MI_X64_SUB_RR, w, loc);
        m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst_val); m.ops[1] = mop_vreg_use(b_raw);
        emit(c, m);
        MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
        sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst_ovf);
        /* Signed overflow -> OF (MCOND_VS); unsigned carry/borrow -> CF (MCOND_CC) */
        sc.ops[1] = mop_cond(is_signed ? MCOND_VS : MCOND_CC);
        emit(c, sc);
        return;
    }

    if (op == PIR_OP_MUL_OVF_S) {
        emit_copy(c, dst_val, a_raw, w, loc);
        MInst m = minst_make(MI_X64_IMUL_RR, w, loc);
        m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst_val); m.ops[1] = mop_vreg_use(b_raw);
        emit(c, m);
        MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
        sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst_ovf); sc.ops[1] = mop_cond(MCOND_VS);
        emit(c, sc);
        return;
    }

    /* MUL_OVF_U: 1-op unsigned mul sets OF=CF=1 iff upper half (rdx) != 0! */
    MInst ma = minst_make(MI_COPY, w, loc);
    ma.op_count = 2u; ma.ops[0] = mop_preg_def(X64_RAX); ma.ops[1] = mop_vreg_use(a_raw);
    emit(c, ma);
    MInst ml = minst_make(MI_X64_MUL_1, w, loc);
    ml.op_count = 3u;
    ml.ops[0]   = mop_vreg_use(b_raw);
    ml.ops[1]   = mop_preg_defuse(X64_RAX);
    ml.ops[2]   = mop_preg_def(X64_RDX);
    emit(c, ml);
    MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
    sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst_ovf); sc.ops[1] = mop_cond(MCOND_VS);
    emit(c, sc);
    MInst cp = minst_make(MI_COPY, w, loc);
    cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst_val); cp.ops[1] = mop_preg_use(X64_RAX);
    emit(c, cp);
}

static void emit_scalar_uitofp(X64ISelCtx *c, MVReg dst, MVReg src, PirType src_ty,
                               PirType dst_ty, uint32_t loc) {
    bool is_f64 = (dst_ty.u.scalar == PIR_SCALAR_F64);
    uint8_t fw = is_f64 ? 8u : 4u;
    MOpcode cvt = is_f64 ? MI_X64_CVTSI2SD : MI_X64_CVTSI2SS;
    if (src_ty.u.scalar != PIR_SCALAR_I64) {
        MVReg z = normalize_int(c, src, src_ty, false, loc);
        if (src_ty.u.scalar == PIR_SCALAR_I32) {
            /* mov r32, r32 zero-extends to 64-bit! */
            MVReg z64 = new_gp(c, 8u);
            MInst mv = minst_make(MI_X64_MOV_RR, 4u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(z64); mv.ops[1] = mop_vreg_use(z);
            emit(c, mv);
            z = z64;
        }
        MInst cv = minst_make(cvt, fw, loc);
        cv.aux_width = 8u;
        cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst); cv.ops[1] = mop_vreg_use(z);
        emit(c, cv);
        return;
    }
    /* Unsigned i64 -> f32/f64 exact IEEE-754 conversion */
    uint32_t big_blk  = mfunc_new_block(c->mf, "u2f_big");
    uint32_t sm_blk   = mfunc_new_block(c->mf, "u2f_sm");
    uint32_t done_blk = mfunc_new_block(c->mf, "u2f_done");

    MInst ts = minst_make(MI_X64_TEST_RR, 8u, loc);
    ts.op_count = 2u; ts.ops[0] = mop_vreg_use(src); ts.ops[1] = mop_vreg_use(src);
    emit(c, ts);
    MInst js = minst_make(MI_X64_JCC, 4u, loc);
    js.op_count = 2u; js.ops[0] = mop_cond(MCOND_MI); js.ops[1] = mop_block(big_blk);
    emit(c, js);
    MInst jsm = minst_make(MI_X64_JMP, 4u, loc);
    jsm.op_count = 1u; jsm.ops[0] = mop_block(sm_blk);
    emit(c, jsm);
    mfunc_add_edge(c->mf, c->cur_mblk, big_blk);
    mfunc_add_edge(c->mf, c->cur_mblk, sm_blk);

    /* sm_blk: src < 2^63 -> single signed cvtsi2sd/ss */
    c->cur_mblk = sm_blk;
    MInst cvs = minst_make(cvt, fw, loc);
    cvs.aux_width = 8u; cvs.op_count = 2u; cvs.ops[0] = mop_vreg_def(dst); cvs.ops[1] = mop_vreg_use(src);
    emit(c, cvs);
    MInst jd1 = minst_make(MI_X64_JMP, 4u, loc);
    jd1.op_count = 1u; jd1.ops[0] = mop_block(done_blk);
    emit(c, jd1);
    mfunc_add_edge(c->mf, sm_blk, done_blk);

    /* big_blk: ((src >> 1) | (src & 1)) -> cvt -> double */
    c->cur_mblk = big_blk;
    MVReg half = new_gp(c, 8u), lsb = new_gp(c, 8u);
    emit_copy(c, half, src, 8u, loc);
    MInst sh = minst_make(MI_X64_SHR_RI, 8u, loc);
    sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(half); sh.ops[1] = mop_imm(1);
    emit(c, sh);
    emit_copy(c, lsb, src, 8u, loc);
    MInst an = minst_make(MI_X64_AND_RI, 8u, loc);
    an.op_count = 2u; an.ops[0] = mop_vreg_defuse(lsb); an.ops[1] = mop_imm(1);
    emit(c, an);
    MInst o = minst_make(MI_X64_OR_RR, 8u, loc);
    o.op_count = 2u; o.ops[0] = mop_vreg_defuse(half); o.ops[1] = mop_vreg_use(lsb);
    emit(c, o);
    MInst cvb = minst_make(cvt, fw, loc);
    cvb.aux_width = 8u; cvb.op_count = 2u; cvb.ops[0] = mop_vreg_def(dst); cvb.ops[1] = mop_vreg_use(half);
    emit(c, cvb);
    MInst ad = minst_make(is_f64 ? MI_X64_ADDSD : MI_X64_ADDSS, fw, loc);
    ad.op_count = 2u; ad.ops[0] = mop_vreg_defuse(dst); ad.ops[1] = mop_vreg_use(dst);
    emit(c, ad);
    MInst jd2 = minst_make(MI_X64_JMP, 4u, loc);
    jd2.op_count = 1u; jd2.ops[0] = mop_block(done_blk);
    emit(c, jd2);
    mfunc_add_edge(c->mf, big_blk, done_blk);

    c->cur_mblk = done_blk;
}

static uint64_t fp_pow2_bits(bool is_f64, int exp, bool neg) {
    if (is_f64) {
        uint64_t s = neg ? ((uint64_t)1u << 63u) : 0u;
        return s | ((uint64_t)(exp + 1023) << 52u);
    }
    uint32_t s = neg ? ((uint32_t)1u << 31u) : 0u;
    return (uint64_t)(s | ((uint32_t)(exp + 127) << 23u));
}

static void emit_scalar_fptoi(X64ISelCtx *c, MVReg dst, MVReg src, PirType src_ty,
                              PirType dst_ty, bool is_signed, uint32_t flags, uint32_t loc) {
    bool is_f64 = (src_ty.u.scalar == PIR_SCALAR_F64);
    uint8_t fw  = is_f64 ? 8u : 4u;
    uint32_t bits = pir_scalar_bits(dst_ty.u.scalar, 8u);
    uint8_t dw  = pir_type_vreg_bytes(dst_ty);
    MOpcode ucomi = is_f64 ? MI_X64_UCOMISD : MI_X64_UCOMISS;
    MOpcode cvt   = is_f64 ? MI_X64_CVTTSD2SI : MI_X64_CVTTSS2SI;

    uint64_t lo_fp_bits = is_signed ? fp_pow2_bits(is_f64, (int)(bits - 1u), true) : 0u;
    uint64_t hi_fp_bits = fp_pow2_bits(is_f64, is_signed ? (int)(bits - 1u) : (int)bits, false);

    uint64_t min_int_val = is_signed
        ? ((bits == 64u) ? ((uint64_t)1u << 63u) : (uint64_t)(-(int64_t)((uint64_t)1u << (bits - 1u))))
        : 0u;
    uint64_t max_int_val = is_signed
        ? (((uint64_t)1u << (bits - 1u)) - 1u)
        : ((bits == 64u) ? UINT64_MAX : (((uint64_t)1u << bits) - 1u));

    if ((flags & PIR_FLAG_TRAP) != 0u) {
        /* Trap on NaN, < lo_bound, or >= hi_excl_bound */
        MInst fn = minst_make(ucomi, fw, loc);
        fn.op_count = 2u; fn.ops[0] = mop_vreg_use(src); fn.ops[1] = mop_vreg_use(src);
        emit(c, fn);
        emit_cond_trap(c, MCOND_P, loc);

        MVReg flo = new_fp(c, fw);
        emit_imm_fp(c, flo, lo_fp_bits, fw, loc);
        MInst cl = minst_make(ucomi, fw, loc);
        cl.op_count = 2u; cl.ops[0] = mop_vreg_use(src); cl.ops[1] = mop_vreg_use(flo);
        emit(c, cl);
        emit_cond_trap(c, MCOND_CC, loc);

        MVReg fhi = new_fp(c, fw);
        emit_imm_fp(c, fhi, hi_fp_bits, fw, loc);
        MInst ch = minst_make(ucomi, fw, loc);
        ch.op_count = 2u; ch.ops[0] = mop_vreg_use(src); ch.ops[1] = mop_vreg_use(fhi);
        emit(c, ch);
        emit_cond_trap(c, MCOND_CS, loc);
    }

    uint32_t sat_done = MBLOCK_NONE;
    if ((flags & PIR_FLAG_SAT) != 0u) {
        /* Saturating bounds: NaN -> 0, < lo -> min_int, >= hi_excl -> max_int */
        uint32_t nan_blk = mfunc_new_block(c->mf, "f2i_nan");
        uint32_t chk_lo  = mfunc_new_block(c->mf, "f2i_clo");
        uint32_t lo_blk  = mfunc_new_block(c->mf, "f2i_lo");
        uint32_t chk_hi  = mfunc_new_block(c->mf, "f2i_chi");
        uint32_t hi_blk  = mfunc_new_block(c->mf, "f2i_hi");
        uint32_t cvt_blk = mfunc_new_block(c->mf, "f2i_cvt");
        sat_done         = mfunc_new_block(c->mf, "f2i_done");

        MInst fn = minst_make(ucomi, fw, loc);
        fn.op_count = 2u; fn.ops[0] = mop_vreg_use(src); fn.ops[1] = mop_vreg_use(src);
        emit(c, fn);
        MInst jp = minst_make(MI_X64_JCC, 4u, loc);
        jp.op_count = 2u; jp.ops[0] = mop_cond(MCOND_P); jp.ops[1] = mop_block(nan_blk);
        emit(c, jp);
        MInst j1 = minst_make(MI_X64_JMP, 4u, loc);
        j1.op_count = 1u; j1.ops[0] = mop_block(chk_lo);
        emit(c, j1);
        mfunc_add_edge(c->mf, c->cur_mblk, nan_blk);
        mfunc_add_edge(c->mf, c->cur_mblk, chk_lo);

        c->cur_mblk = nan_blk;
        emit_imm_gp(c, dst, 0u, dw, loc);
        MInst jn = minst_make(MI_X64_JMP, 4u, loc);
        jn.op_count = 1u; jn.ops[0] = mop_block(sat_done);
        emit(c, jn);
        mfunc_add_edge(c->mf, nan_blk, sat_done);

        c->cur_mblk = chk_lo;
        MVReg flo = new_fp(c, fw);
        emit_imm_fp(c, flo, lo_fp_bits, fw, loc);
        MInst cl = minst_make(ucomi, fw, loc);
        cl.op_count = 2u; cl.ops[0] = mop_vreg_use(src); cl.ops[1] = mop_vreg_use(flo);
        emit(c, cl);
        MInst jb = minst_make(MI_X64_JCC, 4u, loc);
        jb.op_count = 2u; jb.ops[0] = mop_cond(MCOND_CC); jb.ops[1] = mop_block(lo_blk);
        emit(c, jb);
        MInst j2 = minst_make(MI_X64_JMP, 4u, loc);
        j2.op_count = 1u; j2.ops[0] = mop_block(chk_hi);
        emit(c, j2);
        mfunc_add_edge(c->mf, chk_lo, lo_blk);
        mfunc_add_edge(c->mf, chk_lo, chk_hi);

        c->cur_mblk = lo_blk;
        emit_imm_gp(c, dst, min_int_val, dw, loc);
        MInst jl = minst_make(MI_X64_JMP, 4u, loc);
        jl.op_count = 1u; jl.ops[0] = mop_block(sat_done);
        emit(c, jl);
        mfunc_add_edge(c->mf, lo_blk, sat_done);

        c->cur_mblk = chk_hi;
        MVReg fhi = new_fp(c, fw);
        emit_imm_fp(c, fhi, hi_fp_bits, fw, loc);
        MInst ch = minst_make(ucomi, fw, loc);
        ch.op_count = 2u; ch.ops[0] = mop_vreg_use(src); ch.ops[1] = mop_vreg_use(fhi);
        emit(c, ch);
        MInst ja = minst_make(MI_X64_JCC, 4u, loc);
        ja.op_count = 2u; ja.ops[0] = mop_cond(MCOND_CS); ja.ops[1] = mop_block(hi_blk);
        emit(c, ja);
        MInst j3 = minst_make(MI_X64_JMP, 4u, loc);
        j3.op_count = 1u; j3.ops[0] = mop_block(cvt_blk);
        emit(c, j3);
        mfunc_add_edge(c->mf, chk_hi, hi_blk);
        mfunc_add_edge(c->mf, chk_hi, cvt_blk);

        c->cur_mblk = hi_blk;
        emit_imm_gp(c, dst, max_int_val, dw, loc);
        MInst jh = minst_make(MI_X64_JMP, 4u, loc);
        jh.op_count = 1u; jh.ops[0] = mop_block(sat_done);
        emit(c, jh);
        mfunc_add_edge(c->mf, hi_blk, sat_done);

        c->cur_mblk = cvt_blk;
    }

    if (!is_signed && bits == 64u) {
        /* Unsigned i64 conversion: split around 2^63 */
        uint32_t big_blk  = mfunc_new_block(c->mf, "f2u_big");
        uint32_t sm_blk   = mfunc_new_block(c->mf, "f2u_sm");
        uint32_t done_blk = (sat_done != MBLOCK_NONE) ? sat_done : mfunc_new_block(c->mf, "f2u_done");

        MVReg p63 = new_fp(c, fw);
        emit_imm_fp(c, p63, fp_pow2_bits(is_f64, 63, false), fw, loc);
        MInst cp = minst_make(ucomi, fw, loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_use(src); cp.ops[1] = mop_vreg_use(p63);
        emit(c, cp);
        MInst jae = minst_make(MI_X64_JCC, 4u, loc);
        jae.op_count = 2u; jae.ops[0] = mop_cond(MCOND_CS); jae.ops[1] = mop_block(big_blk);
        emit(c, jae);
        MInst jsm = minst_make(MI_X64_JMP, 4u, loc);
        jsm.op_count = 1u; jsm.ops[0] = mop_block(sm_blk);
        emit(c, jsm);
        mfunc_add_edge(c->mf, c->cur_mblk, big_blk);
        mfunc_add_edge(c->mf, c->cur_mblk, sm_blk);

        c->cur_mblk = sm_blk;
        MInst c1 = minst_make(cvt, 8u, loc);
        c1.op_count = 2u; c1.ops[0] = mop_vreg_def(dst); c1.ops[1] = mop_vreg_use(src);
        emit(c, c1);
        MInst jd1 = minst_make(MI_X64_JMP, 4u, loc);
        jd1.op_count = 1u; jd1.ops[0] = mop_block(done_blk);
        emit(c, jd1);
        mfunc_add_edge(c->mf, sm_blk, done_blk);

        c->cur_mblk = big_blk;
        MVReg adj = new_fp(c, fw);
        emit_copy(c, adj, src, fw, loc);
        MInst sb = minst_make(is_f64 ? MI_X64_SUBSD : MI_X64_SUBSS, fw, loc);
        sb.op_count = 2u; sb.ops[0] = mop_vreg_defuse(adj); sb.ops[1] = mop_vreg_use(p63);
        emit(c, sb);
        MInst c2 = minst_make(cvt, 8u, loc);
        c2.op_count = 2u; c2.ops[0] = mop_vreg_def(dst); c2.ops[1] = mop_vreg_use(adj);
        emit(c, c2);
        MVReg hib = new_gp(c, 8u);
        emit_imm_gp(c, hib, (uint64_t)1u << 63u, 8u, loc);
        MInst xo = minst_make(MI_X64_XOR_RR, 8u, loc);
        xo.op_count = 2u; xo.ops[0] = mop_vreg_defuse(dst); xo.ops[1] = mop_vreg_use(hib);
        emit(c, xo);
        MInst jd2 = minst_make(MI_X64_JMP, 4u, loc);
        jd2.op_count = 1u; jd2.ops[0] = mop_block(done_blk);
        emit(c, jd2);
        mfunc_add_edge(c->mf, big_blk, done_blk);

        c->cur_mblk = done_blk;
        return;
    }

    /* Signed (any width) or unsigned i8/i16/i32 -> 64-bit cvttsd2si/cvttss2si handles all cleanly! */
    MVReg tmp64 = (dw == 8u) ? dst : new_gp(c, 8u);
    MInst cv = minst_make(cvt, 8u, loc);
    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(tmp64); cv.ops[1] = mop_vreg_use(src);
    emit(c, cv);
    if (dw != 8u) {
        MInst mv = minst_make(MI_X64_MOV_RR, 4u, loc);
        mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst); mv.ops[1] = mop_vreg_use(tmp64);
        emit(c, mv);
    }

    if (sat_done != MBLOCK_NONE) {
        MInst jd = minst_make(MI_X64_JMP, 4u, loc);
        jd.op_count = 1u; jd.ops[0] = mop_block(sat_done);
        emit(c, jd);
        mfunc_add_edge(c->mf, c->cur_mblk, sat_done);
        c->cur_mblk = sat_done;
    }
}

/* =========================================================================
 * Call Lowering (SysV AMD64 + Compiler-RT __divti3 Adapter)
 * ========================================================================= */

static const MPReg SYSV_GP_ARGS[6] = {
    X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9
};

typedef struct {
    bool     is_reg;
    bool     is_stack_byval;
    MPReg    preg;
    int64_t  stack_off;
    uint8_t  mem_bytes;
    uint64_t byval_size;
    bool     promote_f32_to_f64;
} X64ArgLoc;

static void plan_call_args_x64(const PirType *arg_tys, const uint32_t *arg_aligns,
                               const uint64_t *byval_sizes, const uint32_t *byval_aligns,
                               const bool *force_stack,
                               uint32_t arg_count, bool is_variadic, uint32_t fixed_count,
                               X64ArgLoc *locs, uint64_t *out_stack_bytes,
                               uint32_t *out_gp_used, uint32_t *out_fp_used) {
    uint32_t gp = 0u, fp = 0u;
    uint64_t st_off = 0u;

    for (uint32_t i = 0u; i < arg_count; ++i) {
        memset(&locs[i], 0, sizeof(locs[i]));
        PirType ty = arg_tys[i];
        bool is_va_arg = is_variadic && (i >= fixed_count);

        if (byval_sizes && byval_sizes[i] > 0u) {
            /* SysV AMD64 MEMORY-class struct passed inline on stack; consumes 0 GP/FP regs! */
            uint64_t al = (byval_aligns && byval_aligns[i] > 8u) ? (uint64_t)byval_aligns[i] : 8u;
            uint64_t sz = align_up_u64(byval_sizes[i], 8u);
            st_off = align_up_u64(st_off, al);
            locs[i].is_reg         = false;
            locs[i].is_stack_byval = true;
            locs[i].stack_off      = (int64_t)st_off;
            locs[i].byval_size     = byval_sizes[i];
            st_off += sz;
            continue;
        }

        bool is_fp = (pir_type_rclass(ty) == MREG_CLASS_FP);
        uint8_t mb = pir_type_mem_bytes(ty);
        locs[i].mem_bytes = mb;
        if (is_va_arg && ty.kind == PIR_TYPE_SCALAR && ty.u.scalar == PIR_SCALAR_F32) {
            locs[i].promote_f32_to_f64 = true;
            locs[i].mem_bytes = 8u;
            mb = 8u;
        }

        bool forced = (force_stack && force_stack[i]);
        if (is_fp) {
            if (!forced && fp < 8u) {
                locs[i].is_reg = true;
                locs[i].preg   = X64_XMM(fp++);
            } else {
                uint64_t al = (mb > 8u) ? (uint64_t)mb : 8u;
                if (arg_aligns && arg_aligns[i] > al) al = arg_aligns[i];
                uint64_t sz = (mb > 8u) ? (uint64_t)mb : 8u;
                st_off = align_up_u64(st_off, al);
                locs[i].is_reg    = false;
                locs[i].stack_off = (int64_t)st_off;
                st_off += sz;
            }
        } else {
            if (!forced && gp < 6u) {
                locs[i].is_reg = true;
                locs[i].preg   = SYSV_GP_ARGS[gp++];
            } else {
                uint64_t al = 8u;
                if (arg_aligns && arg_aligns[i] > al) al = arg_aligns[i];
                st_off = align_up_u64(st_off, al);
                locs[i].is_reg    = false;
                locs[i].stack_off = (int64_t)st_off;
                st_off += 8u;
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

static void emit_call_inst(X64ISelCtx *c, const PirInst *inst, uint32_t loc) {
    bool is_ind = (inst->op == PIR_OP_CALL_INDIRECT);
    uint32_t nargs = inst->operand_count - 1u;

    /* Inline SSE2 hardware sqrtsd/sqrtss & fabs/fabsf (IEEE-754 exact). */
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
            if (inst->ty.kind == PIR_TYPE_SCALAR && arg_ty.kind == PIR_TYPE_SCALAR &&
                inst->ty.u.scalar == arg_ty.u.scalar) {
                if ((inst->ty.u.scalar == PIR_SCALAR_F64 && strcmp(cname, "sqrt") == 0) ||
                    (inst->ty.u.scalar == PIR_SCALAR_F32 && strcmp(cname, "sqrtf") == 0)) {
                    uint8_t fw = (inst->ty.u.scalar == PIR_SCALAR_F64) ? 8u : 4u;
                    MVReg src = mat_op(c, &inst->operands[1], loc);
                    MVReg dst = c->val_vreg[inst->results[0]];
                    MInst sq = minst_make(fw == 8u ? MI_X64_SQRTSD : MI_X64_SQRTSS, fw, loc);
                    sq.op_count = 2u; sq.ops[0] = mop_vreg_def(dst); sq.ops[1] = mop_vreg_use(src);
                    emit(c, sq);
                    return;
                }
                if ((inst->ty.u.scalar == PIR_SCALAR_F64 && strcmp(cname, "fabs") == 0) ||
                    (inst->ty.u.scalar == PIR_SCALAR_F32 && strcmp(cname, "fabsf") == 0)) {
                    uint8_t fw = (inst->ty.u.scalar == PIR_SCALAR_F64) ? 8u : 4u;
                    MVReg src = mat_op(c, &inst->operands[1], loc);
                    MVReg dst = c->val_vreg[inst->results[0]];
                    MVReg msk = new_fp(c, fw);
                    emit_imm_fp(c, msk, fw == 8u ? 0x7FFFFFFFFFFFFFFFULL : 0x7FFFFFFFULL, fw, loc);
                    emit_copy(c, dst, src, fw, loc);
                    MInst an = minst_make(MI_X64_PAND, fw, loc);
                    an.op_count = 2u; an.ops[0] = mop_vreg_defuse(dst); an.ops[1] = mop_vreg_use(msk);
                    emit(c, an);
                    return;
                }
            }
        }
    }

    c->mf->has_calls = true;

    /* Native compiler-rt adapter for __divti3 / __udivti3 / __modti3 / __umodti3:
     * SysV AMD64 passes (alo, ahi, blo, bhi) in (rdi, rsi, rdx, rcx) and returns (lo=rax, hi=rdx). */
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
            mv.op_count = 2u; mv.ops[0] = mop_preg_def(SYSV_GP_ARGS[k]); mv.ops[1] = mop_vreg_use(args4[k]);
            emit(c, mv);
        }
        MInst call = minst_make(MI_X64_CALL, 8u, loc);
        call.is_call  = true;
        call.op_count = 1u;
        call.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, call);

        MVReg rlo = new_gp(c, 8u), rhi = new_gp(c, 8u);
        MInst c0 = minst_make(MI_COPY, 8u, loc);
        c0.op_count = 2u; c0.ops[0] = mop_vreg_def(rlo); c0.ops[1] = mop_preg_use(X64_RAX);
        emit(c, c0);
        MInst c1 = minst_make(MI_COPY, 8u, loc);
        c1.op_count = 2u; c1.ops[0] = mop_vreg_def(rhi); c1.ops[1] = mop_preg_use(X64_RDX);
        emit(c, c1);

        MInst s0 = minst_make(MI_X64_MOV_MR, 8u, loc);
        s0.op_count = 3u; s0.ops[0] = mop_vreg_use(rlo); s0.ops[1] = mop_vreg_use(out_ptr); s0.ops[2] = mop_imm(0);
        emit(c, s0);
        MInst s1 = minst_make(MI_X64_MOV_MR, 8u, loc);
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

    /* For PIR_CABI_RET_MULTI_REG, arg 0 is a hidden caller buffer (not passed to callee).
     * For PIR_CABI_RET_SRET_RAX, arg 0 IS passed normally in rdi! */
    bool has_multi_out = (cabi_ret.kind == PIR_CABI_RET_MULTI_REG) && nargs > 0u;
    uint32_t arg_skip  = has_multi_out ? 1u : 0u;
    uint32_t pass_nargs = nargs - arg_skip;
    uint32_t pass_fixed = (fixed_count >= arg_skip) ? (fixed_count - arg_skip) : 0u;

    PirType   *arg_tys     = pass_nargs ? (PirType *)calloc(pass_nargs, sizeof(PirType)) : NULL;
    uint32_t  *arg_aligns  = pass_nargs ? (uint32_t *)calloc(pass_nargs, sizeof(uint32_t)) : NULL;
    uint64_t  *byval_sizes = pass_nargs ? (uint64_t *)calloc(pass_nargs, sizeof(uint64_t)) : NULL;
    uint32_t  *byval_als   = pass_nargs ? (uint32_t *)calloc(pass_nargs, sizeof(uint32_t)) : NULL;
    bool      *force_stk   = pass_nargs ? (bool *)calloc(pass_nargs, sizeof(bool)) : NULL;
    X64ArgLoc *alocs       = pass_nargs ? (X64ArgLoc *)calloc(pass_nargs, sizeof(X64ArgLoc)) : NULL;
    MVReg     *avregs      = pass_nargs ? (MVReg *)calloc(pass_nargs, sizeof(MVReg)) : NULL;
    if (pass_nargs && (!arg_tys || !arg_aligns || !byval_sizes || !byval_als ||
                       !force_stk || !alocs || !avregs)) abort();

    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        const PirOperand *op = &inst->operands[i + 1u + arg_skip];
        arg_tys[i] = op->type;
        if (cal_params && (i + arg_skip) < cal_param_count) {
            arg_aligns[i]  = cal_params[i + arg_skip].abi_stack_align;
            byval_sizes[i] = cal_params[i + arg_skip].abi_byval_size;
            byval_als[i]   = cal_params[i + arg_skip].abi_byval_align;
            force_stk[i]   = cal_params[i + arg_skip].abi_force_stack;
        }
        if (op->call_byval_size > 0u) {
            byval_sizes[i] = op->call_byval_size;
            byval_als[i]   = op->call_byval_align;
        }
    }

    uint64_t stack_bytes = 0u;
    uint32_t fp_used = 0u;
    plan_call_args_x64(arg_tys, arg_aligns, byval_sizes, byval_als, force_stk,
                       pass_nargs, is_variadic, pass_fixed,
                       alocs, &stack_bytes, NULL, &fp_used);

    if (stack_bytes > c->mf->max_out_args_bytes) {
        c->mf->max_out_args_bytes = stack_bytes;
    }

    /* Materialize fn_ptr, hidden_out_v, and all arguments BEFORE touching ABI regs. */
    MVReg fn_ptr_v = MVREG_NONE;
    if (is_ind) fn_ptr_v = mat_op(c, &inst->operands[0], loc);
    MVReg hidden_out_v = MVREG_NONE;
    if (has_multi_out) hidden_out_v = mat_op(c, &inst->operands[1], loc);

    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        MVReg v = mat_op(c, &inst->operands[i + 1u + arg_skip], loc);
        if (alocs[i].promote_f32_to_f64) {
            MVReg d = new_fp(c, 8u);
            MInst cv = minst_make(MI_X64_CVTSS2SD, 8u, loc);
            cv.op_count = 2u; cv.ops[0] = mop_vreg_def(d); cv.ops[1] = mop_vreg_use(v);
            emit(c, cv);
            v = d;
        }
        avregs[i] = v;
    }

    if (stack_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_DOWN, 8u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)stack_bytes);
        emit(c, adj);
    }

    /* 1. Store stack arguments (and copy inline MEMORY byval structs) first. */
    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        if (alocs[i].is_reg) continue;
        if (alocs[i].is_stack_byval) {
            MInst cp = minst_make(MI_COPY_OUTARG_MEM, 8u, loc);
            cp.aux_id   = (uint32_t)alocs[i].byval_size;
            cp.op_count = 2u;
            cp.ops[0]   = mop_vreg_use(avregs[i]);
            cp.ops[1]   = mop_imm(alocs[i].stack_off);
            emit(c, cp);
        } else {
            uint8_t sw = alocs[i].mem_bytes;
            MVReg sv = avregs[i];
            if (pir_type_rclass(arg_tys[i]) == MREG_CLASS_GP && sw < 4u) {
                sv = normalize_int(c, sv, arg_tys[i], false, loc);
                sw = 4u;
            }
            MInst st = minst_make(MI_STORE_OUTARG, sw, loc);
            st.op_count = 2u;
            st.ops[0]   = mop_vreg_use(sv);
            st.ops[1]   = mop_imm(alocs[i].stack_off);
            emit(c, st);
        }
    }

    /* 2. Move register arguments into SysV GP (rdi..r9) / XMM (xmm0..xmm7). */
    for (uint32_t i = 0u; i < pass_nargs; ++i) {
        if (alocs[i].is_reg) {
            uint8_t w = alocs[i].promote_f32_to_f64 ? 8u : pir_type_vreg_bytes(arg_tys[i]);
            MInst mv = minst_make(MI_COPY, w, loc);
            mv.op_count = 2u;
            mv.ops[0]   = mop_preg_def(alocs[i].preg);
            mv.ops[1]   = mop_vreg_use(avregs[i]);
            emit(c, mv);
        }
    }

    /* 3. SysV AMD64 variadic rule: set al = count of XMM argument registers (0..8). */
    if (is_variadic) {
        MInst al = minst_make(MI_X64_MOV_RI, 4u, loc);
        al.op_count = 2u; al.ops[0] = mop_preg_def(X64_RAX); al.ops[1] = mop_imm((int64_t)fp_used);
        emit(c, al);
    }

    /* 4. Emit direct or indirect call (indirect target via r10 scratch, never clobbers args or al!). */
    if (is_ind) {
        MInst mv = minst_make(MI_COPY, 8u, loc);
        mv.op_count = 2u; mv.ops[0] = mop_preg_def(X64_SCRATCH_GP0); mv.ops[1] = mop_vreg_use(fn_ptr_v);
        emit(c, mv);
        MInst ci = minst_make(MI_X64_CALL_IND, 8u, loc);
        ci.is_call  = true;
        ci.op_count = 1u;
        ci.ops[0]   = mop_preg_use(X64_SCRATCH_GP0);
        emit(c, ci);
    } else {
        MInst cl = minst_make(MI_X64_CALL, 8u, loc);
        cl.is_call  = true;
        cl.op_count = 1u;
        cl.ops[0]   = mop_sym(inst->operands[0].u.sym.name, 0, true, false, false);
        emit(c, cl);
    }

    if (stack_bytes > 0u) {
        MInst adj = minst_make(MI_CALL_ADJ_UP, 8u, loc);
        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)stack_bytes);
        emit(c, adj);
    }

    /* 5. Capture return value(s). */
    if (cabi_ret.kind == PIR_CABI_RET_MULTI_REG && hidden_out_v != MVREG_NONE) {
        MVReg rvs[4] = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
        uint32_t gp_ri = 0u, fp_ri = 0u;
        for (uint32_t k = 0u; k < cabi_ret.reg_count; ++k) {
            PirType rt = cabi_ret.regs[k];
            MPReg rreg;
            if (pir_type_rclass(rt) == MREG_CLASS_FP) {
                rreg = X64_XMM(fp_ri++);
            } else {
                rreg = (gp_ri++ == 0u) ? X64_RAX : X64_RDX;
            }
            uint8_t w = pir_type_vreg_bytes(rt);
            rvs[k] = new_vreg_for_type(c, rt);
            MInst cp = minst_make(MI_COPY, w, loc);
            cp.op_count = 2u; cp.ops[0] = mop_vreg_def(rvs[k]); cp.ops[1] = mop_preg_use(rreg);
            emit(c, cp);
        }
        uint64_t rem = cabi_ret.agg_size;
        uint64_t off = 0u;
        for (uint32_t k = 0u; k < cabi_ret.reg_count && rem > 0u; ++k) {
            PirType rt = cabi_ret.regs[k];
            bool is_fp = (pir_type_rclass(rt) == MREG_CLASS_FP);
            uint8_t mb = pir_type_mem_bytes(rt);
            if (rem >= mb) {
                MInst st = minst_make(is_fp ? MI_X64_MOVX_MR : MI_X64_MOV_MR, mb, loc);
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
                static const uint8_t CHUNKS[3] = { 4u, 2u, 1u };
                for (uint32_t ci = 0u; ci < 3u && rem > 0u; ++ci) {
                    uint8_t cw = CHUNKS[ci];
                    while (rem >= cw) {
                        MVReg sv = cur;
                        if (cur_shift > 0u) {
                            sv = new_gp(c, 8u);
                            emit_copy(c, sv, cur, 8u, loc);
                            MInst sh = minst_make(MI_X64_SHR_RI, 8u, loc);
                            sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(sv); sh.ops[1] = mop_imm((int64_t)cur_shift);
                            emit(c, sh);
                        }
                        MInst st = minst_make(MI_X64_MOV_MR, cw, loc);
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
        MPReg rreg = (pir_type_rclass(ret_ty) == MREG_CLASS_FP) ? X64_XMM(0u) : X64_RAX;
        uint8_t w  = pir_type_vreg_bytes(ret_ty);
        MInst cp = minst_make(MI_COPY, w, loc);
        cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(rreg);
        emit(c, cp);
    }

    free(arg_tys);
    free(arg_aligns);
    free(byval_sizes);
    free(byval_als);
    free(force_stk);
    free(alocs);
    free(avregs);
}

/* =========================================================================
 * Addressing Mode Matcher & Pattern Matchers
 * ========================================================================= */

static bool pir_oper_val_eq(PirOperand a, PirOperand b) {
    return a.kind == PIR_OPER_VAL && b.kind == PIR_OPER_VAL && a.u.val == b.u.val;
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
        shr_id = d0->def_inst; shl_id = d1->def_inst; shr_ins = i0; shl_ins = i1;
    } else if (i0->op == PIR_OP_SHL && i1->op == PIR_OP_SHR) {
        shl_id = d0->def_inst; shr_id = d1->def_inst; shl_ins = i0; shr_ins = i1;
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
            *out_shr_id = shr_id; *out_shl_id = shl_id;
            *out_x = shr_ins->operands[0]; *out_is_imm = true; *out_imm = (uint32_t)ra;
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
                        *out_shr_id = shr_id; *out_shl_id = shl_id;
                        *out_x = shr_ins->operands[0]; *out_is_imm = false;
                        *out_imm = 0u; *out_reg_n = shr_ins->operands[1];
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

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

    PirOperand a = fc->operands[0], b = fc->operands[1];
    PirOperand tv = sel->operands[1], fv = sel->operands[2];
    bool same_order = pir_oper_val_eq(tv, a) && pir_oper_val_eq(fv, b);
    bool swap_order = pir_oper_val_eq(tv, b) && pir_oper_val_eq(fv, a);
    if (!same_order && !swap_order) return false;

    bool fast_ok = ((sel->flags & PIR_FLAG_FAST) != 0u) ||
                   ((fc->flags & PIR_FLAG_FAST) != 0u) ||
                   (a.kind == PIR_OPER_VAL && val_has_fast_fp_origin(fn, bid, a.u.val, 4u)) ||
                   (b.kind == PIR_OPER_VAL && val_has_fast_fp_origin(fn, bid, b.u.val, 4u));
    if (!fast_ok) return false;

    bool is_max = (is_gt && same_order) || (is_lt && swap_order);
    *out_fcmp_id = vd->def_inst;
    if (el == PIR_SCALAR_F64) {
        *out_mop = is_max ? MI_X64_MAXPD : MI_X64_MINPD;
    } else {
        *out_mop = is_max ? MI_X64_MAXPS : MI_X64_MINPS;
    }
    *out_a = a;
    *out_b = b;
    return true;
}

static bool try_match_mem_addr(X64ISelCtx *c, const PirOperand *ptr_op,
                               MVReg *out_base, int64_t *out_disp,
                               bool *out_has_idx, MVReg *out_idx, uint8_t *out_shift,
                               uint32_t loc) {
    *out_has_idx = false;
    *out_disp    = 0;
    *out_shift   = 0u;

    if (ptr_op->kind == PIR_OPER_VAL) {
        PirValueId vid = ptr_op->u.val;
        const PirValueDef *vd = &c->fn->values.data[vid];
        if (vd->def_inst != PIR_INVALID_ID &&
            c->inst_fused[vd->def_inst] &&
            c->fn->insts.data[vd->def_inst].op == PIR_OP_PTR_ADD) {
            const PirInst *pa = &c->fn->insts.data[vd->def_inst];
            if (pa->operands[0].kind == PIR_OPER_VAL && pa->operands[1].kind == PIR_OPER_IMM_INT) {
                int64_t off = (pa->ty.u.scalar == PIR_SCALAR_I32)
                              ? (int64_t)(int32_t)eval_imm_u64(&pa->operands[1])
                              : (int64_t)eval_imm_u64(&pa->operands[1]);
                if (off >= INT32_MIN && off <= INT32_MAX) {
                    *out_base = c->val_vreg[pa->operands[0].u.val];
                    *out_disp = off;
                    return true;
                }
            }
            if (pa->operands[0].kind == PIR_OPER_VAL && pa->operands[1].kind == PIR_OPER_VAL &&
                pa->ty.kind == PIR_TYPE_SCALAR && pa->ty.u.scalar == PIR_SCALAR_I64 &&
                c->val_uses[vid] == 1u) {
                PirValueId idx_v = pa->operands[1].u.val;
                uint8_t sh_amt = 0u;
                const PirValueDef *ivd = &c->fn->values.data[idx_v];
                if (ivd->def_inst != PIR_INVALID_ID &&
                    c->inst_fused[ivd->def_inst] &&
                    c->fn->insts.data[ivd->def_inst].op == PIR_OP_SHL &&
                    c->fn->insts.data[ivd->def_inst].operands[0].kind == PIR_OPER_VAL &&
                    c->fn->insts.data[ivd->def_inst].operands[1].kind == PIR_OPER_IMM_INT &&
                    c->val_uses[idx_v] == 1u) {
                    uint64_t sh = eval_imm_u64(&c->fn->insts.data[ivd->def_inst].operands[1]);
                    if (sh <= 3u) {
                        sh_amt = (uint8_t)sh;
                        idx_v  = c->fn->insts.data[ivd->def_inst].operands[0].u.val;
                    }
                }
                *out_base    = c->val_vreg[pa->operands[0].u.val];
                *out_has_idx = true;
                *out_idx     = c->val_vreg[idx_v];
                *out_shift   = sh_amt;
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

static bool isel_inst(X64ISelCtx *c, PirInstId iid, PirBlockId cur_pir_blk) {
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
                emit_imm_fp(c, dst0, op->u.imm_float.bits, pir_type_vreg_bytes(inst->ty), loc);
            } else {
                MVReg s = mat_op(c, op, loc);
                emit_copy(c, dst0, s, pir_type_vreg_bytes(inst->ty), loc);
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
                PirScalarType el = pir_vec_elem_scalar(vt);
                uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);

                if (inst->op == PIR_OP_ADD || inst->op == PIR_OP_SUB) {
                    MOpcode mop = (inst->op == PIR_OP_ADD)
                        ? ((el == PIR_SCALAR_I8) ? MI_X64_PADDB
                           : (el == PIR_SCALAR_I16) ? MI_X64_PADDW
                           : (el == PIR_SCALAR_I32) ? MI_X64_PADDD : MI_X64_PADDQ)
                        : ((el == PIR_SCALAR_I8) ? MI_X64_PSUBB
                           : (el == PIR_SCALAR_I16) ? MI_X64_PSUBW
                           : (el == PIR_SCALAR_I32) ? MI_X64_PSUBD : MI_X64_PSUBQ);
                    emit_copy(c, dst0, a, vb, loc);
                    MInst v = minst_make(mop, vb, loc);
                    v.op_count = 2u; v.ops[0] = mop_vreg_defuse(dst0); v.ops[1] = mop_vreg_use(b);
                    emit(c, v);
                    return true;
                }
                if (el == PIR_SCALAR_I16) {
                    emit_copy(c, dst0, a, vb, loc);
                    MInst v = minst_make(MI_X64_PMULLW, vb, loc);
                    v.op_count = 2u; v.ops[0] = mop_vreg_defuse(dst0); v.ops[1] = mop_vreg_use(b);
                    emit(c, v);
                    return true;
                }
                /* Lane-wise vector integer mul (i8, i32, i64) under baseline SSE2 */
                uint32_t lanes = pir_vec_lanes(vt);
                uint8_t eb = pir_type_mem_bytes(pir_type_scalar(el));
                uint8_t ew = (eb == 8u) ? 8u : 4u;
                uint32_t sa = get_vec_tmp_slot(c, 0u), sb = get_vec_tmp_slot(c, 1u), sd = get_vec_tmp_slot(c, 2u);
                emit_vec_store_slot(c, a, sa, vb, loc);
                emit_vec_store_slot(c, b, sb, vb, loc);
                MVReg ba = emit_slot_addr(c, sa, 0, loc);
                MVReg bb = emit_slot_addr(c, sb, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    int64_t off = (int64_t)(l * eb);
                    MVReg la = new_gp(c, ew), lb = new_gp(c, ew);
                    MInst lda = minst_make(MI_X64_MOV_RM, eb, loc);
                    lda.op_count = 3u; lda.ops[0] = mop_vreg_def(la); lda.ops[1] = mop_vreg_use(ba); lda.ops[2] = mop_imm(off);
                    emit(c, lda);
                    MInst ldb = minst_make(MI_X64_MOV_RM, eb, loc);
                    ldb.op_count = 3u; ldb.ops[0] = mop_vreg_def(lb); ldb.ops[1] = mop_vreg_use(bb); ldb.ops[2] = mop_imm(off);
                    emit(c, ldb);
                    MInst ml = minst_make(MI_X64_IMUL_RR, ew, loc);
                    ml.op_count = 2u; ml.ops[0] = mop_vreg_defuse(la); ml.ops[1] = mop_vreg_use(lb);
                    emit(c, ml);
                    MInst st = minst_make(MI_X64_MOV_MR, eb, loc);
                    st.op_count = 3u; st.ops[0] = mop_vreg_use(la); st.ops[1] = mop_vreg_use(bd); st.ops[2] = mop_imm(off);
                    emit(c, st);
                }
                emit_vec_load_slot(c, dst0, sd, vb, loc);
                return true;
            }

            /* Scalar add/sub/mul */
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if ((inst->flags & (PIR_FLAG_TRAP_S | PIR_FLAG_TRAP_U)) != 0u) {
                MVReg b = mat_op(c, &inst->operands[1], loc);
                bool is_s = (inst->flags & PIR_FLAG_TRAP_S) != 0u;
                PirOpcode ovf_op = (inst->op == PIR_OP_ADD) ? (is_s ? PIR_OP_ADD_OVF_S : PIR_OP_ADD_OVF_U)
                                 : (inst->op == PIR_OP_SUB) ? (is_s ? PIR_OP_SUB_OVF_S : PIR_OP_SUB_OVF_U)
                                                            : (is_s ? PIR_OP_MUL_OVF_S : PIR_OP_MUL_OVF_U);
                MVReg ovf = new_gp(c, 4u);
                emit_scalar_ovf(c, ovf_op, inst->ty, a, b, dst0, ovf, loc);
                MInst ts = minst_make(MI_X64_TEST_RR, 4u, loc);
                ts.op_count = 2u; ts.ops[0] = mop_vreg_use(ovf); ts.ops[1] = mop_vreg_use(ovf);
                emit(c, ts);
                emit_cond_trap(c, MCOND_NE, loc);
                return true;
            }

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
                    emit_copy(c, dst0, vx, w, loc);
                    if (r_is_imm) {
                        MInst ror = minst_make(MI_X64_ROR_RI, w, loc);
                        ror.op_count = 2u; ror.ops[0] = mop_vreg_defuse(dst0); ror.ops[1] = mop_imm((int64_t)r_imm);
                        emit(c, ror);
                    } else {
                        MVReg vn = mat_op(c, &rn, loc);
                        MInst mc = minst_make(MI_COPY, 4u, loc);
                        mc.op_count = 2u; mc.ops[0] = mop_preg_def(X64_RCX); mc.ops[1] = mop_vreg_use(vn);
                        emit(c, mc);
                        MInst ror = minst_make(MI_X64_ROR_CL, w, loc);
                        ror.op_count = 2u; ror.ops[0] = mop_vreg_defuse(dst0); ror.ops[1] = mop_preg_use(X64_RCX);
                        emit(c, ror);
                    }
                    return true;
                }
            }

            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint64_t imm = eval_imm_u64(&inst->operands[1]);
                if (fits_simm32(imm, w)) {
                    int64_t simm = sign_ext_imm(imm, w);
                    if (inst->op == PIR_OP_MUL) {
                        MInst m = minst_make(MI_X64_IMUL_RRI, w, loc);
                        m.op_count = 3u;
                        m.ops[0] = mop_vreg_def(dst0); m.ops[1] = mop_vreg_use(a); m.ops[2] = mop_imm(simm);
                        emit(c, m);
                        return true;
                    }
                    if (inst->op == PIR_OP_ADD && w == 8u) {
                        /* 3-address non-destructive lea dst, [a + imm] */
                        MInst lea = minst_make(MI_X64_LEA, 8u, loc);
                        lea.op_count = 3u;
                        lea.ops[0] = mop_vreg_def(dst0); lea.ops[1] = mop_vreg_use(a); lea.ops[2] = mop_imm(simm);
                        emit(c, lea);
                        return true;
                    }
                    emit_copy(c, dst0, a, w, loc);
                    MInst m = minst_make(inst->op == PIR_OP_ADD ? MI_X64_ADD_RI : MI_X64_SUB_RI, w, loc);
                    m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst0); m.ops[1] = mop_imm(simm);
                    emit(c, m);
                    return true;
                }
            }

            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (inst->op == PIR_OP_ADD && w == 8u) {
                MInst lea = minst_make(MI_X64_LEA_IDX, 8u, loc);
                lea.aux_width = 0u;
                lea.op_count = 4u;
                lea.ops[0] = mop_vreg_def(dst0); lea.ops[1] = mop_vreg_use(a);
                lea.ops[2] = mop_vreg_use(b);    lea.ops[3] = mop_imm(0);
                emit(c, lea);
                return true;
            }
            emit_copy(c, dst0, a, w, loc);
            MOpcode mop = (inst->op == PIR_OP_ADD) ? MI_X64_ADD_RR
                        : (inst->op == PIR_OP_SUB) ? MI_X64_SUB_RR : MI_X64_IMUL_RR;
            MInst m = minst_make(mop, w, loc);
            m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst0); m.ops[1] = mop_vreg_use(b);
            emit(c, m);
            return true;
        }

        case PIR_OP_DIV_S: case PIR_OP_DIV_U:
        case PIR_OP_REM_S: case PIR_OP_REM_U: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType vt = inst->ty.u.vec;
                PirScalarType el = pir_vec_elem_scalar(vt);
                PirType el_ty = pir_type_scalar(el);
                uint32_t lanes = pir_vec_lanes(vt);
                uint8_t eb = pir_type_mem_bytes(el_ty);
                uint8_t ew = (eb == 8u) ? 8u : 4u;
                uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
                bool is_s = (inst->op == PIR_OP_DIV_S || inst->op == PIR_OP_REM_S);
                bool is_r = (inst->op == PIR_OP_REM_S || inst->op == PIR_OP_REM_U);
                MVReg va = mat_op(c, &inst->operands[0], loc);
                MVReg vb_r = mat_op(c, &inst->operands[1], loc);
                uint32_t sa = get_vec_tmp_slot(c, 0u), sb = get_vec_tmp_slot(c, 1u), sd = get_vec_tmp_slot(c, 2u);
                emit_vec_store_slot(c, va, sa, vb, loc);
                emit_vec_store_slot(c, vb_r, sb, vb, loc);
                MVReg ba = emit_slot_addr(c, sa, 0, loc);
                MVReg bb = emit_slot_addr(c, sb, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    int64_t off = (int64_t)(l * eb);
                    MVReg la = new_gp(c, ew), lb = new_gp(c, ew), lr = new_gp(c, ew);
                    MInst lda = minst_make(MI_X64_MOV_RM, eb, loc);
                    lda.is_signed = is_s; lda.op_count = 3u;
                    lda.ops[0] = mop_vreg_def(la); lda.ops[1] = mop_vreg_use(ba); lda.ops[2] = mop_imm(off);
                    emit(c, lda);
                    MInst ldb = minst_make(MI_X64_MOV_RM, eb, loc);
                    ldb.is_signed = is_s; ldb.op_count = 3u;
                    ldb.ops[0] = mop_vreg_def(lb); ldb.ops[1] = mop_vreg_use(bb); ldb.ops[2] = mop_imm(off);
                    emit(c, ldb);

                    MInst ma = minst_make(MI_COPY, ew, loc);
                    ma.op_count = 2u; ma.ops[0] = mop_preg_def(X64_RAX); ma.ops[1] = mop_vreg_use(la);
                    emit(c, ma);
                    if (is_s) {
                        MInst cqo = minst_make(MI_X64_CQO, ew, loc);
                        cqo.op_count = 2u; cqo.ops[0] = mop_preg_def(X64_RDX); cqo.ops[1] = mop_preg_use(X64_RAX);
                        emit(c, cqo);
                    } else {
                        MInst zd = minst_make(MI_X64_MOV_RI, 4u, loc);
                        zd.op_count = 2u; zd.ops[0] = mop_preg_def(X64_RDX); zd.ops[1] = mop_imm(0);
                        emit(c, zd);
                    }
                    MInst dv = minst_make(is_s ? MI_X64_IDIV : MI_X64_DIV, ew, loc);
                    dv.op_count = 3u; dv.ops[0] = mop_vreg_use(lb);
                    dv.ops[1] = mop_preg_defuse(X64_RAX); dv.ops[2] = mop_preg_defuse(X64_RDX);
                    emit(c, dv);
                    MInst res = minst_make(MI_COPY, ew, loc);
                    res.op_count = 2u; res.ops[0] = mop_vreg_def(lr); res.ops[1] = mop_preg_use(is_r ? X64_RDX : X64_RAX);
                    emit(c, res);

                    MInst st = minst_make(MI_X64_MOV_MR, eb, loc);
                    st.op_count = 3u; st.ops[0] = mop_vreg_use(lr); st.ops[1] = mop_vreg_use(bd); st.ops[2] = mop_imm(off);
                    emit(c, st);
                }
                emit_vec_load_slot(c, dst0, sd, vb, loc);
                return true;
            }
            emit_scalar_div_rem(c, inst, dst0, loc);
            return true;
        }

        case PIR_OP_ADD_OVF_S: case PIR_OP_ADD_OVF_U:
        case PIR_OP_SUB_OVF_S: case PIR_OP_SUB_OVF_U:
        case PIR_OP_MUL_OVF_S: case PIR_OP_MUL_OVF_U: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            emit_scalar_ovf(c, inst->op, inst->ty, a, b, dst0, dst1, loc);
            return true;
        }

        case PIR_OP_MUL_WIDE_U:
        case PIR_OP_MUL_WIDE_S: {
            bool is_s = (inst->op == PIR_OP_MUL_WIDE_S);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            MInst ma = minst_make(MI_COPY, w, loc);
            ma.op_count = 2u; ma.ops[0] = mop_preg_def(X64_RAX); ma.ops[1] = mop_vreg_use(a);
            emit(c, ma);
            MInst ml = minst_make(is_s ? MI_X64_IMUL_1 : MI_X64_MUL_1, w, loc);
            ml.op_count = 3u;
            ml.ops[0]   = mop_vreg_use(b);
            ml.ops[1]   = mop_preg_defuse(X64_RAX);
            ml.ops[2]   = mop_preg_def(X64_RDX);
            emit(c, ml);
            MInst clo = minst_make(MI_COPY, w, loc);
            clo.op_count = 2u; clo.ops[0] = mop_vreg_def(dst0); clo.ops[1] = mop_preg_use(X64_RAX);
            emit(c, clo);
            MInst chi = minst_make(MI_COPY, w, loc);
            chi.op_count = 2u; chi.ops[0] = mop_vreg_def(dst1); chi.ops[1] = mop_preg_use(X64_RDX);
            emit(c, chi);
            return true;
        }

        case PIR_OP_ADD_CARRY:
        case PIR_OP_SUB_BORROW: {
            bool is_add = (inst->op == PIR_OP_ADD_CARRY);
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            MVReg cin = normalize_int(c, mat_op(c, &inst->operands[2], loc),
                                      pir_type_scalar(PIR_SCALAR_I8), false, loc);
            if (bits < 32u) {
                MVReg na = normalize_int(c, a, inst->ty, false, loc);
                MVReg nb = normalize_int(c, b, inst->ty, false, loc);
                MVReg t = new_gp(c, 4u);
                emit_copy(c, t, na, 4u, loc);
                MInst m1 = minst_make(is_add ? MI_X64_ADD_RR : MI_X64_SUB_RR, 4u, loc);
                m1.op_count = 2u; m1.ops[0] = mop_vreg_defuse(t); m1.ops[1] = mop_vreg_use(nb);
                emit(c, m1);
                MInst m2 = minst_make(is_add ? MI_X64_ADD_RR : MI_X64_SUB_RR, 4u, loc);
                m2.op_count = 2u; m2.ops[0] = mop_vreg_defuse(t); m2.ops[1] = mop_vreg_use(cin);
                emit(c, m2);
                emit_copy(c, dst0, t, 4u, loc);
                emit_copy(c, dst1, t, 4u, loc);
                MInst sh = minst_make(MI_X64_SHR_RI, 4u, loc);
                sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(dst1);
                sh.ops[1] = mop_imm(is_add ? (int64_t)bits : 31);
                emit(c, sh);
                return true;
            }
            /* Set CF = (cin != 0) via `neg cin_copy`, then adc/sbb dst0, b; setc dst1! */
            emit_copy(c, dst0, a, w, loc);
            MVReg cf_tmp = new_gp(c, 4u);
            emit_copy(c, cf_tmp, cin, 4u, loc);
            MInst ng = minst_make(MI_X64_NEG, 4u, loc);
            ng.op_count = 1u; ng.ops[0] = mop_vreg_defuse(cf_tmp);
            emit(c, ng);
            MInst ac = minst_make(is_add ? MI_X64_ADC_RR : MI_X64_SBB_RR, w, loc);
            ac.op_count = 2u; ac.ops[0] = mop_vreg_defuse(dst0); ac.ops[1] = mop_vreg_use(b);
            emit(c, ac);
            MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
            sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst1); sc.ops[1] = mop_cond(MCOND_CC);
            emit(c, sc);
            return true;
        }

        case PIR_OP_AND: case PIR_OP_OR: case PIR_OP_XOR: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                uint8_t vb = (uint8_t)(pir_vec_bits(inst->ty.u.vec) / 8u);
                MVReg a = mat_op(c, &inst->operands[0], loc);
                MVReg b = mat_op(c, &inst->operands[1], loc);
                emit_copy(c, dst0, a, vb, loc);
                MOpcode mop = (inst->op == PIR_OP_AND) ? MI_X64_PAND
                            : (inst->op == PIR_OP_OR)  ? MI_X64_POR : MI_X64_PXOR;
                MInst v = minst_make(mop, vb, loc);
                v.op_count = 2u; v.ops[0] = mop_vreg_defuse(dst0); v.ops[1] = mop_vreg_use(b);
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
                    emit_copy(c, dst0, vx, w, loc);
                    if (r_is_imm) {
                        MInst ror = minst_make(MI_X64_ROR_RI, w, loc);
                        ror.op_count = 2u; ror.ops[0] = mop_vreg_defuse(dst0); ror.ops[1] = mop_imm((int64_t)r_imm);
                        emit(c, ror);
                    } else {
                        MVReg vn = mat_op(c, &rn, loc);
                        MInst mc = minst_make(MI_COPY, 4u, loc);
                        mc.op_count = 2u; mc.ops[0] = mop_preg_def(X64_RCX); mc.ops[1] = mop_vreg_use(vn);
                        emit(c, mc);
                        MInst ror = minst_make(MI_X64_ROR_CL, w, loc);
                        ror.op_count = 2u; ror.ops[0] = mop_vreg_defuse(dst0); ror.ops[1] = mop_preg_use(X64_RCX);
                        emit(c, ror);
                    }
                    return true;
                }
            }
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint64_t imm = eval_imm_u64(&inst->operands[1]);
                if (fits_simm32(imm, w)) {
                    emit_copy(c, dst0, a, w, loc);
                    MOpcode mop = (inst->op == PIR_OP_AND) ? MI_X64_AND_RI
                                : (inst->op == PIR_OP_OR)  ? MI_X64_OR_RI : MI_X64_XOR_RI;
                    MInst m = minst_make(mop, w, loc);
                    m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst0); m.ops[1] = mop_imm(sign_ext_imm(imm, w));
                    emit(c, m);
                    return true;
                }
            }
            MVReg b = mat_op(c, &inst->operands[1], loc);
            emit_copy(c, dst0, a, w, loc);
            MOpcode mop = (inst->op == PIR_OP_AND) ? MI_X64_AND_RR
                        : (inst->op == PIR_OP_OR)  ? MI_X64_OR_RR : MI_X64_XOR_RR;
            MInst m = minst_make(mop, w, loc);
            m.op_count = 2u; m.ops[0] = mop_vreg_defuse(dst0); m.ops[1] = mop_vreg_use(b);
            emit(c, m);
            return true;
        }

        case PIR_OP_SHL: case PIR_OP_SHR: case PIR_OP_SAR: {
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                /* Per-lane vector shifts (baseline SSE2 has no per-lane variable shifts). */
                PirVecType vt = inst->ty.u.vec;
                PirScalarType el = pir_vec_elem_scalar(vt);
                uint32_t lanes = pir_vec_lanes(vt);
                uint32_t ebits = pir_scalar_bits(el, 8u);
                uint8_t eb = (uint8_t)(ebits / 8u);
                uint8_t ew = (eb == 8u) ? 8u : 4u;
                uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
                MVReg va = mat_op(c, &inst->operands[0], loc);
                MVReg vb_r = mat_op(c, &inst->operands[1], loc);
                uint32_t sa = get_vec_tmp_slot(c, 0u), sb = get_vec_tmp_slot(c, 1u), sd = get_vec_tmp_slot(c, 2u);
                emit_vec_store_slot(c, va, sa, vb, loc);
                emit_vec_store_slot(c, vb_r, sb, vb, loc);
                MVReg ba = emit_slot_addr(c, sa, 0, loc);
                MVReg bb = emit_slot_addr(c, sb, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                MOpcode sop = (inst->op == PIR_OP_SHL) ? MI_X64_SHL_CL
                            : (inst->op == PIR_OP_SHR) ? MI_X64_SHR_CL : MI_X64_SAR_CL;
                for (uint32_t l = 0u; l < lanes; ++l) {
                    int64_t off = (int64_t)(l * eb);
                    MVReg la = new_gp(c, ew), lb = new_gp(c, 4u);
                    MInst lda = minst_make(MI_X64_MOV_RM, eb, loc);
                    lda.is_signed = (inst->op == PIR_OP_SAR);
                    lda.op_count = 3u; lda.ops[0] = mop_vreg_def(la); lda.ops[1] = mop_vreg_use(ba); lda.ops[2] = mop_imm(off);
                    emit(c, lda);
                    MInst ldb = minst_make(MI_X64_MOV_RM, eb, loc);
                    ldb.op_count = 3u; ldb.ops[0] = mop_vreg_def(lb); ldb.ops[1] = mop_vreg_use(bb); ldb.ops[2] = mop_imm(off);
                    emit(c, ldb);
                    MInst mk = minst_make(MI_X64_AND_RI, 4u, loc);
                    mk.op_count = 2u; mk.ops[0] = mop_vreg_defuse(lb); mk.ops[1] = mop_imm((int64_t)(ebits - 1u));
                    emit(c, mk);
                    MInst mc = minst_make(MI_COPY, 4u, loc);
                    mc.op_count = 2u; mc.ops[0] = mop_preg_def(X64_RCX); mc.ops[1] = mop_vreg_use(lb);
                    emit(c, mc);
                    MInst sh = minst_make(sop, ew, loc);
                    sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(la); sh.ops[1] = mop_preg_use(X64_RCX);
                    emit(c, sh);
                    MInst st = minst_make(MI_X64_MOV_MR, eb, loc);
                    st.op_count = 3u; st.ops[0] = mop_vreg_use(la); st.ops[1] = mop_vreg_use(bd); st.ops[2] = mop_imm(off);
                    emit(c, st);
                }
                emit_vec_load_slot(c, dst0, sd, vb, loc);
                return true;
            }
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (inst->op == PIR_OP_SHR)      a = normalize_int(c, a, inst->ty, false, loc);
            else if (inst->op == PIR_OP_SAR) a = normalize_int(c, a, inst->ty, true, loc);

            if ((inst->flags & PIR_FLAG_TRAP) != 0u) {
                MVReg b = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
                MInst cmp = minst_make(MI_X64_CMP_RI, w, loc);
                cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(b); cmp.ops[1] = mop_imm((int64_t)bits);
                emit(c, cmp);
                emit_cond_trap(c, MCOND_CS, loc);
                emit_copy(c, dst0, a, w, loc);
                MInst mc = minst_make(MI_COPY, 4u, loc);
                mc.op_count = 2u; mc.ops[0] = mop_preg_def(X64_RCX); mc.ops[1] = mop_vreg_use(b);
                emit(c, mc);
                MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_X64_SHL_CL
                            : (inst->op == PIR_OP_SHR) ? MI_X64_SHR_CL : MI_X64_SAR_CL;
                MInst sh = minst_make(mop, w, loc);
                sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(dst0); sh.ops[1] = mop_preg_use(X64_RCX);
                emit(c, sh);
                return true;
            }

            if (inst->operands[1].kind == PIR_OPER_IMM_INT) {
                uint64_t amt = eval_imm_u64(&inst->operands[1]) & (uint64_t)(bits - 1u);
                emit_copy(c, dst0, a, w, loc);
                if (amt > 0u) {
                    MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_X64_SHL_RI
                                : (inst->op == PIR_OP_SHR) ? MI_X64_SHR_RI : MI_X64_SAR_RI;
                    MInst sh = minst_make(mop, w, loc);
                    sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(dst0); sh.ops[1] = mop_imm((int64_t)amt);
                    emit(c, sh);
                }
                return true;
            }
            MVReg b = mat_op(c, &inst->operands[1], loc);
            if (bits < 32u) {
                MVReg bm = new_gp(c, 4u);
                emit_copy(c, bm, b, 4u, loc);
                MInst an = minst_make(MI_X64_AND_RI, 4u, loc);
                an.op_count = 2u; an.ops[0] = mop_vreg_defuse(bm); an.ops[1] = mop_imm((int64_t)(bits - 1u));
                emit(c, an);
                b = bm;
            }
            emit_copy(c, dst0, a, w, loc);
            MInst mc = minst_make(MI_COPY, 4u, loc);
            mc.op_count = 2u; mc.ops[0] = mop_preg_def(X64_RCX); mc.ops[1] = mop_vreg_use(b);
            emit(c, mc);
            MOpcode mop = (inst->op == PIR_OP_SHL) ? MI_X64_SHL_CL
                        : (inst->op == PIR_OP_SHR) ? MI_X64_SHR_CL : MI_X64_SAR_CL;
            MInst sh = minst_make(mop, w, loc);
            sh.op_count = 2u; sh.ops[0] = mop_vreg_defuse(dst0); sh.ops[1] = mop_preg_use(X64_RCX);
            emit(c, sh);
            return true;
        }

        case PIR_OP_CLZ: {
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, false, loc);
            MVReg r = new_gp(c, w);
            MInst bsr = minst_make(MI_X64_BSR, w, loc);
            bsr.op_count = 2u; bsr.ops[0] = mop_vreg_def(r); bsr.ops[1] = mop_vreg_use(a);
            emit(c, bsr);
            MInst xo = minst_make(MI_X64_XOR_RI, w, loc);
            xo.op_count = 2u; xo.ops[0] = mop_vreg_defuse(r); xo.ops[1] = mop_imm((int64_t)(w * 8u - 1u));
            emit(c, xo);
            if ((inst->flags & PIR_FLAG_NONZERO) == 0u) {
                MVReg zval = new_gp(c, w);
                emit_imm_gp(c, zval, (uint64_t)(w * 8u), w, loc);
                MInst ts = minst_make(MI_X64_TEST_RR, w, loc);
                ts.op_count = 2u; ts.ops[0] = mop_vreg_use(a); ts.ops[1] = mop_vreg_use(a);
                emit(c, ts);
                MInst cm = minst_make(MI_X64_CMOVCC, w, loc);
                cm.op_count = 3u;
                cm.ops[0] = mop_vreg_defuse(r); cm.ops[1] = mop_vreg_use(zval); cm.ops[2] = mop_cond(MCOND_EQ);
                emit(c, cm);
            }
            if (bits < 32u) {
                MInst sb = minst_make(MI_X64_SUB_RI, 4u, loc);
                sb.op_count = 2u; sb.ops[0] = mop_vreg_defuse(r); sb.ops[1] = mop_imm((int64_t)(32u - bits));
                emit(c, sb);
            }
            emit_copy(c, dst0, r, w, loc);
            return true;
        }

        case PIR_OP_CTZ: {
            uint32_t bits = pir_scalar_bits(inst->ty.u.scalar, 8u);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg a = mat_op(c, &inst->operands[0], loc);
            if (bits < 32u) {
                MVReg ao = new_gp(c, 4u);
                emit_copy(c, ao, a, 4u, loc);
                MInst o = minst_make(MI_X64_OR_RI, 4u, loc);
                o.op_count = 2u; o.ops[0] = mop_vreg_defuse(ao); o.ops[1] = mop_imm((int64_t)((uint64_t)1u << bits));
                emit(c, o);
                MInst bsf = minst_make(MI_X64_BSF, 4u, loc);
                bsf.op_count = 2u; bsf.ops[0] = mop_vreg_def(dst0); bsf.ops[1] = mop_vreg_use(ao);
                emit(c, bsf);
                return true;
            }
            MVReg r = new_gp(c, w);
            MInst bsf = minst_make(MI_X64_BSF, w, loc);
            bsf.op_count = 2u; bsf.ops[0] = mop_vreg_def(r); bsf.ops[1] = mop_vreg_use(a);
            emit(c, bsf);
            if ((inst->flags & PIR_FLAG_NONZERO) == 0u) {
                MVReg zval = new_gp(c, w);
                emit_imm_gp(c, zval, (uint64_t)bits, w, loc);
                MInst ts = minst_make(MI_X64_TEST_RR, w, loc);
                ts.op_count = 2u; ts.ops[0] = mop_vreg_use(a); ts.ops[1] = mop_vreg_use(a);
                emit(c, ts);
                MInst cm = minst_make(MI_X64_CMOVCC, w, loc);
                cm.op_count = 3u;
                cm.ops[0] = mop_vreg_defuse(r); cm.ops[1] = mop_vreg_use(zval); cm.ops[2] = mop_cond(MCOND_EQ);
                emit(c, cm);
            }
            emit_copy(c, dst0, r, w, loc);
            return true;
        }

        case PIR_OP_POPCNT: {
            /* Portable branchless SWAR popcnt (works on every baseline x86-64 CPU & Rosetta 2). */
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg x = new_gp(c, w);
            MVReg a = normalize_int(c, mat_op(c, &inst->operands[0], loc), inst->ty, false, loc);
            emit_copy(c, x, a, w, loc);
            MVReg t = new_gp(c, w), m = new_gp(c, w);

            /* x = x - ((x >> 1) & 0x5555...) */
            emit_copy(c, t, x, w, loc);
            MInst s1 = minst_make(MI_X64_SHR_RI, w, loc);
            s1.op_count = 2u; s1.ops[0] = mop_vreg_defuse(t); s1.ops[1] = mop_imm(1);
            emit(c, s1);
            emit_imm_gp(c, m, w == 8u ? 0x5555555555555555ULL : 0x55555555ULL, w, loc);
            MInst a1 = minst_make(MI_X64_AND_RR, w, loc);
            a1.op_count = 2u; a1.ops[0] = mop_vreg_defuse(t); a1.ops[1] = mop_vreg_use(m);
            emit(c, a1);
            MInst sb = minst_make(MI_X64_SUB_RR, w, loc);
            sb.op_count = 2u; sb.ops[0] = mop_vreg_defuse(x); sb.ops[1] = mop_vreg_use(t);
            emit(c, sb);

            /* x = (x & 0x3333...) + ((x >> 2) & 0x3333...) */
            emit_imm_gp(c, m, w == 8u ? 0x3333333333333333ULL : 0x33333333ULL, w, loc);
            emit_copy(c, t, x, w, loc);
            MInst s2 = minst_make(MI_X64_SHR_RI, w, loc);
            s2.op_count = 2u; s2.ops[0] = mop_vreg_defuse(t); s2.ops[1] = mop_imm(2);
            emit(c, s2);
            MInst a2a = minst_make(MI_X64_AND_RR, w, loc);
            a2a.op_count = 2u; a2a.ops[0] = mop_vreg_defuse(x); a2a.ops[1] = mop_vreg_use(m);
            emit(c, a2a);
            MInst a2b = minst_make(MI_X64_AND_RR, w, loc);
            a2b.op_count = 2u; a2b.ops[0] = mop_vreg_defuse(t); a2b.ops[1] = mop_vreg_use(m);
            emit(c, a2b);
            MInst ad1 = minst_make(MI_X64_ADD_RR, w, loc);
            ad1.op_count = 2u; ad1.ops[0] = mop_vreg_defuse(x); ad1.ops[1] = mop_vreg_use(t);
            emit(c, ad1);

            /* x = ((x + (x >> 4)) & 0x0F0F...) * 0x0101... >> (w*8 - 8) */
            emit_copy(c, t, x, w, loc);
            MInst s3 = minst_make(MI_X64_SHR_RI, w, loc);
            s3.op_count = 2u; s3.ops[0] = mop_vreg_defuse(t); s3.ops[1] = mop_imm(4);
            emit(c, s3);
            MInst ad2 = minst_make(MI_X64_ADD_RR, w, loc);
            ad2.op_count = 2u; ad2.ops[0] = mop_vreg_defuse(x); ad2.ops[1] = mop_vreg_use(t);
            emit(c, ad2);
            emit_imm_gp(c, m, w == 8u ? 0x0F0F0F0F0F0F0F0FULL : 0x0F0F0F0FULL, w, loc);
            MInst a3 = minst_make(MI_X64_AND_RR, w, loc);
            a3.op_count = 2u; a3.ops[0] = mop_vreg_defuse(x); a3.ops[1] = mop_vreg_use(m);
            emit(c, a3);
            emit_imm_gp(c, m, w == 8u ? 0x0101010101010101ULL : 0x01010101ULL, w, loc);
            MInst ml = minst_make(MI_X64_IMUL_RR, w, loc);
            ml.op_count = 2u; ml.ops[0] = mop_vreg_defuse(x); ml.ops[1] = mop_vreg_use(m);
            emit(c, ml);
            MInst s4 = minst_make(MI_X64_SHR_RI, w, loc);
            s4.op_count = 2u; s4.ops[0] = mop_vreg_defuse(x); s4.ops[1] = mop_imm(w == 8u ? 56 : 24);
            emit(c, s4);
            emit_copy(c, dst0, x, w, loc);
            return true;
        }

        case PIR_OP_BSWAP: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            emit_copy(c, dst0, a, w, loc);
            MInst bs = minst_make(MI_X64_BSWAP, inst->ty.u.scalar == PIR_SCALAR_I16 ? 2u : w, loc);
            bs.op_count = 1u; bs.ops[0] = mop_vreg_defuse(dst0);
            emit(c, bs);
            return true;
        }

        case PIR_OP_FADD: case PIR_OP_FSUB: case PIR_OP_FMUL: case PIR_OP_FDIV: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            MVReg b = mat_op(c, &inst->operands[1], loc);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            emit_copy(c, dst0, a, w, loc);
            MOpcode mop;
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                bool is_f64 = (pir_vec_elem_scalar(inst->ty.u.vec) == PIR_SCALAR_F64);
                mop = (inst->op == PIR_OP_FADD) ? (is_f64 ? MI_X64_ADDPD : MI_X64_ADDPS)
                    : (inst->op == PIR_OP_FSUB) ? (is_f64 ? MI_X64_SUBPD : MI_X64_SUBPS)
                    : (inst->op == PIR_OP_FMUL) ? (is_f64 ? MI_X64_MULPD : MI_X64_MULPS)
                                                : (is_f64 ? MI_X64_DIVPD : MI_X64_DIVPS);
            } else {
                bool is_f64 = (inst->ty.u.scalar == PIR_SCALAR_F64);
                mop = (inst->op == PIR_OP_FADD) ? (is_f64 ? MI_X64_ADDSD : MI_X64_ADDSS)
                    : (inst->op == PIR_OP_FSUB) ? (is_f64 ? MI_X64_SUBSD : MI_X64_SUBSS)
                    : (inst->op == PIR_OP_FMUL) ? (is_f64 ? MI_X64_MULSD : MI_X64_MULSS)
                                                : (is_f64 ? MI_X64_DIVSD : MI_X64_DIVSS);
            }
            MInst f = minst_make(mop, w, loc);
            f.op_count = 2u; f.ops[0] = mop_vreg_defuse(dst0); f.ops[1] = mop_vreg_use(b);
            emit(c, f);
            return true;
        }

        case PIR_OP_FNEG: {
            MVReg a = mat_op(c, &inst->operands[0], loc);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            MVReg msk = new_fp(c, w);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                bool is_f64 = (pir_vec_elem_scalar(inst->ty.u.vec) == PIR_SCALAR_F64);
                MVReg sgp = new_gp(c, is_f64 ? 8u : 4u);
                emit_imm_gp(c, sgp, is_f64 ? 0x8000000000000000ULL : 0x80000000ULL, is_f64 ? 8u : 4u, loc);
                MVReg sfp = new_fp(c, is_f64 ? 8u : 4u);
                MInst g2x = minst_make(MI_X64_MOV_G2X, is_f64 ? 8u : 4u, loc);
                g2x.op_count = 2u; g2x.ops[0] = mop_vreg_def(sfp); g2x.ops[1] = mop_vreg_use(sgp);
                emit(c, g2x);
                emit_vec_splat(c, msk, sfp, inst->ty.u.vec, loc);
            } else {
                bool is_f64 = (inst->ty.u.scalar == PIR_SCALAR_F64);
                emit_imm_fp(c, msk, is_f64 ? 0x8000000000000000ULL : 0x80000000ULL, w, loc);
            }
            emit_copy(c, dst0, a, w, loc);
            MInst x = minst_make(MI_X64_PXOR, w, loc);
            x.op_count = 2u; x.ops[0] = mop_vreg_defuse(dst0); x.ops[1] = mop_vreg_use(msk);
            emit(c, x);
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
            MInst sc = minst_make(MI_X64_SETCC, 4u, loc);
            sc.op_count = 2u; sc.ops[0] = mop_vreg_def(dst0); sc.ops[1] = mop_cond(int_cmp_to_cond(inst->op));
            emit(c, sc);
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
            MOpcode ucomi = (fw == 8u) ? MI_X64_UCOMISD : MI_X64_UCOMISS;
            MCond sc;
            bool swap = false;
            if (x64_fcmp_single(inst->op, &sc, &swap)) {
                MInst fc = minst_make(ucomi, fw, loc);
                fc.op_count = 2u;
                fc.ops[0] = mop_vreg_use(swap ? b : a);
                fc.ops[1] = mop_vreg_use(swap ? a : b);
                emit(c, fc);
                MInst s = minst_make(MI_X64_SETCC, 4u, loc);
                s.op_count = 2u; s.ops[0] = mop_vreg_def(dst0); s.ops[1] = mop_cond(sc);
                emit(c, s);
            } else if (inst->op == PIR_OP_FCMP_OEQ) {
                /* OEQ: ZF=1 && PF=0 -> sete & setnp */
                MInst fc = minst_make(ucomi, fw, loc);
                fc.op_count = 2u; fc.ops[0] = mop_vreg_use(a); fc.ops[1] = mop_vreg_use(b);
                emit(c, fc);
                MVReg t1 = new_gp(c, 4u), t2 = new_gp(c, 4u);
                MInst s1 = minst_make(MI_X64_SETCC, 4u, loc);
                s1.op_count = 2u; s1.ops[0] = mop_vreg_def(t1); s1.ops[1] = mop_cond(MCOND_EQ);
                emit(c, s1);
                MInst s2 = minst_make(MI_X64_SETCC, 4u, loc);
                s2.op_count = 2u; s2.ops[0] = mop_vreg_def(t2); s2.ops[1] = mop_cond(MCOND_NP);
                emit(c, s2);
                emit_copy(c, dst0, t1, 4u, loc);
                MInst an = minst_make(MI_X64_AND_RR, 4u, loc);
                an.op_count = 2u; an.ops[0] = mop_vreg_defuse(dst0); an.ops[1] = mop_vreg_use(t2);
                emit(c, an);
            } else {
                /* UNE: ZF=0 || PF=1 -> setne | setp */
                MInst fc = minst_make(ucomi, fw, loc);
                fc.op_count = 2u; fc.ops[0] = mop_vreg_use(a); fc.ops[1] = mop_vreg_use(b);
                emit(c, fc);
                MVReg t1 = new_gp(c, 4u), t2 = new_gp(c, 4u);
                MInst s1 = minst_make(MI_X64_SETCC, 4u, loc);
                s1.op_count = 2u; s1.ops[0] = mop_vreg_def(t1); s1.ops[1] = mop_cond(MCOND_NE);
                emit(c, s1);
                MInst s2 = minst_make(MI_X64_SETCC, 4u, loc);
                s2.op_count = 2u; s2.ops[0] = mop_vreg_def(t2); s2.ops[1] = mop_cond(MCOND_P);
                emit(c, s2);
                emit_copy(c, dst0, t1, 4u, loc);
                MInst o = minst_make(MI_X64_OR_RR, 4u, loc);
                o.op_count = 2u; o.ops[0] = mop_vreg_defuse(dst0); o.ops[1] = mop_vreg_use(t2);
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
                    uint8_t vb = pir_type_vreg_bytes(inst->ty);
                    MVReg va = mat_op(c, &fa, loc);
                    MVReg vb_r = mat_op(c, &fb, loc);
                    emit_copy(c, dst0, va, vb, loc);
                    MInst v = minst_make(fmop, vb, loc);
                    v.op_count = 2u; v.ops[0] = mop_vreg_defuse(dst0); v.ops[1] = mop_vreg_use(vb_r);
                    emit(c, v);
                    return true;
                }
                MVReg cond = mat_op(c, &inst->operands[0], loc);
                MVReg tv   = mat_op(c, &inst->operands[1], loc);
                MVReg fv   = mat_op(c, &inst->operands[2], loc);
                emit_vec_select(c, dst0, cond, tv, fv, pir_type_vreg_bytes(inst->ty), loc);
                return true;
            }
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            bool is_fp = (pir_type_rclass(inst->ty) == MREG_CLASS_FP);
            uint32_t before_mat = c->mf->blocks.data[c->cur_mblk].insts.len;
            MVReg tv = mat_op(c, &inst->operands[1], loc);
            MVReg fv = mat_op(c, &inst->operands[2], loc);
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
                    bool flags_live = mat_clean && !is_fp && (c->last_flags_cmp_id == vd->def_inst);
                    if (ci->op >= PIR_OP_CMP_EQ && ci->op <= PIR_OP_CMP_GE_U) {
                        if (!flags_live) emit_scalar_int_cmp_flags(c, ci, loc);
                        sel_cond = int_cmp_to_cond(ci->op);
                        fused_cmp = true;
                        next_flags_id = vd->def_inst;
                    } else if (ci->op >= PIR_OP_FCMP_OEQ && ci->op <= PIR_OP_FCMP_UGE) {
                        bool swap = false;
                        if (x64_fcmp_single(ci->op, &sel_cond, &swap)) {
                            if (!flags_live) {
                                MVReg fa = mat_op(c, &ci->operands[0], loc);
                                MVReg fb = mat_op(c, &ci->operands[1], loc);
                                uint8_t fw = pir_type_vreg_bytes(ci->ty);
                                MInst fc = minst_make(fw == 8u ? MI_X64_UCOMISD : MI_X64_UCOMISS, fw, loc);
                                fc.op_count = 2u;
                                fc.ops[0] = mop_vreg_use(swap ? fb : fa);
                                fc.ops[1] = mop_vreg_use(swap ? fa : fb);
                                emit(c, fc);
                            }
                            fused_cmp = true;
                            next_flags_id = vd->def_inst;
                        }
                    }
                }
            }
            if (!fused_cmp) {
                MVReg cond = mat_op(c, &inst->operands[0], loc);
                PirType ti8 = pir_type_scalar(PIR_SCALAR_I8);
                MVReg cn = is_already_zext_subword(c->fn, &inst->operands[0], ti8)
                           ? cond : normalize_int(c, cond, ti8, false, loc);
                MInst ts = minst_make(MI_X64_TEST_RR, 4u, loc);
                ts.op_count = 2u; ts.ops[0] = mop_vreg_use(cn); ts.ops[1] = mop_vreg_use(cn);
                emit(c, ts);
            }

            if (is_fp) {
                /* x86-64 has no scalar FP cmov: branchless bit-mask or tiny branch select! */
                c->last_flags_cmp_id = PIR_INVALID_ID;
                uint32_t t_blk = mfunc_new_block(c->mf, "fsel_t");
                uint32_t f_blk = mfunc_new_block(c->mf, "fsel_f");
                uint32_t d_blk = mfunc_new_block(c->mf, "fsel_d");
                MInst jc = minst_make(MI_X64_JCC, 4u, loc);
                jc.op_count = 2u; jc.ops[0] = mop_cond(sel_cond); jc.ops[1] = mop_block(t_blk);
                emit(c, jc);
                MInst jf = minst_make(MI_X64_JMP, 4u, loc);
                jf.op_count = 1u; jf.ops[0] = mop_block(f_blk);
                emit(c, jf);
                mfunc_add_edge(c->mf, c->cur_mblk, t_blk);
                mfunc_add_edge(c->mf, c->cur_mblk, f_blk);

                c->cur_mblk = t_blk;
                emit_copy(c, dst0, tv, w, loc);
                MInst jd1 = minst_make(MI_X64_JMP, 4u, loc);
                jd1.op_count = 1u; jd1.ops[0] = mop_block(d_blk);
                emit(c, jd1);
                mfunc_add_edge(c->mf, t_blk, d_blk);

                c->cur_mblk = f_blk;
                emit_copy(c, dst0, fv, w, loc);
                MInst jd2 = minst_make(MI_X64_JMP, 4u, loc);
                jd2.op_count = 1u; jd2.ops[0] = mop_block(d_blk);
                emit(c, jd2);
                mfunc_add_edge(c->mf, f_blk, d_blk);

                c->cur_mblk = d_blk;
            } else {
                c->last_flags_cmp_id = next_flags_id;
                /* Copy fv -> dst0 (mov does not touch flags!), then cmov<sel_cond> dst0, tv. */
                emit_copy(c, dst0, fv, w, loc);
                MInst cm = minst_make(MI_X64_CMOVCC, w, loc);
                cm.op_count = 3u;
                cm.ops[0] = mop_vreg_defuse(dst0); cm.ops[1] = mop_vreg_use(tv); cm.ops[2] = mop_cond(sel_cond);
                emit(c, cm);
            }
            return true;
        }

        case PIR_OP_ZEXT:
        case PIR_OP_SEXT:
        case PIR_OP_TRUNC: {
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType svt = src_ty.u.vec, dvt = inst->ty.u.vec;
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                uint32_t lanes = pir_vec_lanes(dvt);
                uint8_t seb = pir_type_mem_bytes(sel_ty);
                uint8_t deb = pir_type_mem_bytes(del_ty);
                uint8_t svb = (uint8_t)(pir_vec_bits(svt) / 8u);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint32_t ss = get_vec_tmp_slot(c, 0u), sd = get_vec_tmp_slot(c, 1u);
                emit_vec_store_slot(c, src, ss, svb, loc);
                MVReg bs = emit_slot_addr(c, ss, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_gp(c, seb == 8u ? 8u : 4u);
                    MInst ld = minst_make(MI_X64_MOV_RM, seb, loc);
                    ld.is_signed = (inst->op == PIR_OP_SEXT);
                    ld.op_count = 3u; ld.ops[0] = mop_vreg_def(ls); ld.ops[1] = mop_vreg_use(bs);
                    ld.ops[2] = mop_imm((int64_t)(l * seb));
                    emit(c, ld);
                    MVReg out_v = ls;
                    if (inst->op == PIR_OP_SEXT && deb == 8u && seb < 8u) {
                        out_v = new_gp(c, 8u);
                        MInst sx = minst_make(MI_X64_MOVSX, 8u, loc);
                        sx.aux_width = 4u; /* ls already sign-extended to 32-bit by movsx */
                        sx.op_count = 2u; sx.ops[0] = mop_vreg_def(out_v); sx.ops[1] = mop_vreg_use(ls);
                        emit(c, sx);
                    } else if (inst->op == PIR_OP_ZEXT && deb == 8u && seb == 4u) {
                        out_v = new_gp(c, 8u);
                        MInst zx = minst_make(MI_X64_MOV_RR, 4u, loc);
                        zx.op_count = 2u; zx.ops[0] = mop_vreg_def(out_v); zx.ops[1] = mop_vreg_use(ls);
                        emit(c, zx);
                    }
                    MInst st = minst_make(MI_X64_MOV_MR, deb, loc);
                    st.op_count = 3u; st.ops[0] = mop_vreg_use(out_v); st.ops[1] = mop_vreg_use(bd);
                    st.ops[2] = mop_imm((int64_t)(l * deb));
                    emit(c, st);
                }
                emit_vec_load_slot(c, dst0, sd, dvb, loc);
                return true;
            }
            uint8_t dw = pir_type_vreg_bytes(inst->ty);
            if (inst->op == PIR_OP_TRUNC) {
                MInst mv = minst_make(MI_X64_MOV_RR, 4u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
                emit(c, mv);
            } else if (inst->op == PIR_OP_ZEXT) {
                if (src_ty.u.scalar == PIR_SCALAR_I8 || src_ty.u.scalar == PIR_SCALAR_I16) {
                    MInst e = minst_make(MI_X64_MOVZX, 4u, loc);
                    e.aux_width = (src_ty.u.scalar == PIR_SCALAR_I8) ? 1u : 2u;
                    e.op_count = 2u; e.ops[0] = mop_vreg_def(dst0); e.ops[1] = mop_vreg_use(src);
                    emit(c, e);
                } else {
                    /* i32 -> i64 zero-ext: 32-bit mov clears upper 32 bits! */
                    MInst mv = minst_make(MI_X64_MOV_RR, 4u, loc);
                    mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
                    emit(c, mv);
                }
            } else {
                MInst e = minst_make(MI_X64_MOVSX, dw, loc);
                e.aux_width = pir_type_mem_bytes(src_ty);
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
                uint8_t svb = (uint8_t)(pir_vec_bits(svt) / 8u);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint8_t sw  = (inst->op == PIR_OP_FPEXT) ? 4u : 8u;
                uint8_t dw  = (inst->op == PIR_OP_FPEXT) ? 8u : 4u;
                uint32_t ss = get_vec_tmp_slot(c, 0u), sd = get_vec_tmp_slot(c, 1u);
                emit_vec_store_slot(c, src, ss, svb, loc);
                MVReg bs = emit_slot_addr(c, ss, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_fp(c, sw), ld = new_fp(c, dw);
                    MInst fl = minst_make(MI_X64_MOVX_RM, sw, loc);
                    fl.op_count = 3u; fl.ops[0] = mop_vreg_def(ls); fl.ops[1] = mop_vreg_use(bs);
                    fl.ops[2] = mop_imm((int64_t)(l * sw));
                    emit(c, fl);
                    MInst cv = minst_make(inst->op == PIR_OP_FPEXT ? MI_X64_CVTSS2SD : MI_X64_CVTSD2SS, dw, loc);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(ld); cv.ops[1] = mop_vreg_use(ls);
                    emit(c, cv);
                    MInst fs = minst_make(MI_X64_MOVX_MR, dw, loc);
                    fs.op_count = 3u; fs.ops[0] = mop_vreg_use(ld); fs.ops[1] = mop_vreg_use(bd);
                    fs.ops[2] = mop_imm((int64_t)(l * dw));
                    emit(c, fs);
                }
                emit_vec_load_slot(c, dst0, sd, dvb, loc);
                return true;
            }
            MInst cv = minst_make(inst->op == PIR_OP_FPEXT ? MI_X64_CVTSS2SD : MI_X64_CVTSD2SS,
                                  pir_type_vreg_bytes(inst->ty), loc);
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
                if (is_s && svt == PIR_VEC_V4I32 && dvt == PIR_VEC_V4F32) {
                    MInst cv = minst_make(MI_X64_CVTDQ2PS, 16u, loc);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(src);
                    emit(c, cv);
                    return true;
                }
                uint32_t lanes = pir_vec_lanes(dvt);
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                uint8_t seb = pir_type_mem_bytes(sel_ty);
                uint8_t deb = pir_type_mem_bytes(del_ty);
                uint8_t svb = (uint8_t)(pir_vec_bits(svt) / 8u);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint32_t ss = get_vec_tmp_slot(c, 0u), sd = get_vec_tmp_slot(c, 1u);
                emit_vec_store_slot(c, src, ss, svb, loc);
                MVReg bs = emit_slot_addr(c, ss, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_gp(c, seb == 8u ? 8u : 4u);
                    MInst ld = minst_make(MI_X64_MOV_RM, seb, loc);
                    ld.is_signed = is_s; ld.op_count = 3u;
                    ld.ops[0] = mop_vreg_def(ls); ld.ops[1] = mop_vreg_use(bs); ld.ops[2] = mop_imm((int64_t)(l * seb));
                    emit(c, ld);
                    MVReg lf = new_fp(c, deb);
                    if (is_s) {
                        MInst cv = minst_make(deb == 8u ? MI_X64_CVTSI2SD : MI_X64_CVTSI2SS, deb, loc);
                        cv.aux_width = (seb == 8u) ? 8u : 4u;
                        cv.op_count = 2u; cv.ops[0] = mop_vreg_def(lf); cv.ops[1] = mop_vreg_use(ls);
                        emit(c, cv);
                    } else {
                        emit_scalar_uitofp(c, lf, ls, sel_ty, del_ty, loc);
                    }
                    MInst fs = minst_make(MI_X64_MOVX_MR, deb, loc);
                    fs.op_count = 3u; fs.ops[0] = mop_vreg_use(lf); fs.ops[1] = mop_vreg_use(bd);
                    fs.ops[2] = mop_imm((int64_t)(l * deb));
                    emit(c, fs);
                }
                emit_vec_load_slot(c, dst0, sd, dvb, loc);
                return true;
            }
            if (is_s) {
                MVReg ns = normalize_int(c, src, src_ty, true, loc);
                uint8_t fw = pir_type_vreg_bytes(inst->ty);
                MInst cv = minst_make(fw == 8u ? MI_X64_CVTSI2SD : MI_X64_CVTSI2SS, fw, loc);
                cv.aux_width = pir_type_vreg_bytes(src_ty);
                cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(ns);
                emit(c, cv);
            } else {
                emit_scalar_uitofp(c, dst0, src, src_ty, inst->ty, loc);
            }
            return true;
        }

        case PIR_OP_FPTOSI:
        case PIR_OP_FPTOUI: {
            bool is_s = (inst->op == PIR_OP_FPTOSI);
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            if (inst->ty.kind == PIR_TYPE_VECTOR) {
                PirVecType svt = src_ty.u.vec, dvt = inst->ty.u.vec;
                if (is_s && svt == PIR_VEC_V4F32 && dvt == PIR_VEC_V4I32 &&
                    (inst->flags & (PIR_FLAG_SAT | PIR_FLAG_TRAP)) == 0u) {
                    MInst cv = minst_make(MI_X64_CVTTPS2DQ, 16u, loc);
                    cv.op_count = 2u; cv.ops[0] = mop_vreg_def(dst0); cv.ops[1] = mop_vreg_use(src);
                    emit(c, cv);
                    return true;
                }
                uint32_t lanes = pir_vec_lanes(dvt);
                PirType sel_ty = pir_type_scalar(pir_vec_elem_scalar(svt));
                PirType del_ty = pir_type_scalar(pir_vec_elem_scalar(dvt));
                uint8_t seb = pir_type_mem_bytes(sel_ty);
                uint8_t deb = pir_type_mem_bytes(del_ty);
                uint8_t svb = (uint8_t)(pir_vec_bits(svt) / 8u);
                uint8_t dvb = (uint8_t)(pir_vec_bits(dvt) / 8u);
                uint32_t ss = get_vec_tmp_slot(c, 0u), sd = get_vec_tmp_slot(c, 1u);
                emit_vec_store_slot(c, src, ss, svb, loc);
                MVReg bs = emit_slot_addr(c, ss, 0, loc);
                MVReg bd = emit_slot_addr(c, sd, 0, loc);
                for (uint32_t l = 0u; l < lanes; ++l) {
                    MVReg ls = new_fp(c, seb);
                    MInst fl = minst_make(MI_X64_MOVX_RM, seb, loc);
                    fl.op_count = 3u; fl.ops[0] = mop_vreg_def(ls); fl.ops[1] = mop_vreg_use(bs);
                    fl.ops[2] = mop_imm((int64_t)(l * seb));
                    emit(c, fl);
                    MVReg li = new_gp(c, deb == 8u ? 8u : 4u);
                    emit_scalar_fptoi(c, li, ls, sel_ty, del_ty, is_s, inst->flags, loc);
                    MInst st = minst_make(MI_X64_MOV_MR, deb, loc);
                    st.op_count = 3u; st.ops[0] = mop_vreg_use(li); st.ops[1] = mop_vreg_use(bd);
                    st.ops[2] = mop_imm((int64_t)(l * deb));
                    emit(c, st);
                }
                emit_vec_load_slot(c, dst0, sd, dvb, loc);
                return true;
            }
            emit_scalar_fptoi(c, dst0, src, src_ty, inst->ty, is_s, inst->flags, loc);
            return true;
        }

        case PIR_OP_BITCAST: {
            PirType src_ty = inst->operands[0].type;
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MRegClass src_rc = pir_type_rclass(src_ty);
            MRegClass dst_rc = pir_type_rclass(inst->ty);
            uint8_t w = pir_type_vreg_bytes(inst->ty);
            if (src_rc == dst_rc) {
                emit_copy(c, dst0, src, w, loc);
            } else if (src_rc == MREG_CLASS_GP && dst_rc == MREG_CLASS_FP) {
                MInst fm = minst_make(MI_X64_MOV_G2X, w, loc);
                fm.op_count = 2u; fm.ops[0] = mop_vreg_def(dst0); fm.ops[1] = mop_vreg_use(src);
                emit(c, fm);
            } else {
                MInst fm = minst_make(MI_X64_MOV_X2G, w, loc);
                fm.op_count = 2u; fm.ops[0] = mop_vreg_def(dst0); fm.ops[1] = mop_vreg_use(src);
                emit(c, fm);
            }
            return true;
        }

        case PIR_OP_PTRTOINT: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            MInst mv = minst_make(MI_X64_MOV_RR, pir_type_vreg_bytes(inst->ty), loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
            emit(c, mv);
            return true;
        }

        case PIR_OP_INTTOPTR: {
            MVReg src = mat_op(c, &inst->operands[0], loc);
            uint8_t sw = pir_type_vreg_bytes(inst->operands[0].type);
            MInst mv = minst_make(MI_X64_MOV_RR, sw, loc);
            mv.op_count = 2u; mv.ops[0] = mop_vreg_def(dst0); mv.ops[1] = mop_vreg_use(src);
            emit(c, mv);
            return true;
        }

        case PIR_OP_SPLAT: {
            MVReg s = mat_op(c, &inst->operands[0], loc);
            emit_vec_splat(c, dst0, s, inst->ty.u.vec, loc);
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
            uint32_t lanes = pir_vec_lanes(vt);
            uint8_t eb = pir_type_mem_bytes(pir_type_scalar(pir_vec_elem_scalar(vt)));
            uint8_t vb = (uint8_t)(pir_vec_bits(vt) / 8u);
            MVReg va = mat_op(c, &inst->operands[0], loc);
            MVReg vb_r = mat_op(c, &inst->operands[1], loc);

            /* Fast path: 4-lane 32-bit intra-vector shuffle -> single pshufd! */
            if (lanes == 4u && eb == 4u) {
                bool all_a = true, all_b = true;
                for (uint32_t l = 0u; l < 4u; ++l) {
                    if (inst->shuffle_mask[l] >= 4u) all_a = false;
                    else                             all_b = false;
                }
                if (all_a || all_b) {
                    uint8_t imm = 0u;
                    for (uint32_t l = 0u; l < 4u; ++l) {
                        uint8_t m = inst->shuffle_mask[l] & 3u;
                        imm |= (uint8_t)(m << (l * 2u));
                    }
                    MInst sh = minst_make(MI_X64_PSHUFD, 16u, loc);
                    sh.aux_width = imm;
                    sh.op_count = 2u; sh.ops[0] = mop_vreg_def(dst0); sh.ops[1] = mop_vreg_use(all_a ? va : vb_r);
                    emit(c, sh);
                    return true;
                }
            }

            uint32_t sa = get_vec_tmp_slot(c, 0u), sb = get_vec_tmp_slot(c, 1u), sd = get_vec_tmp_slot(c, 2u);
            emit_vec_store_slot(c, va, sa, vb, loc);
            emit_vec_store_slot(c, vb_r, sb, vb, loc);
            MVReg ba = emit_slot_addr(c, sa, 0, loc);
            MVReg bb = emit_slot_addr(c, sb, 0, loc);
            MVReg bd = emit_slot_addr(c, sd, 0, loc);
            for (uint32_t l = 0u; l < lanes; ++l) {
                uint8_t m = inst->shuffle_mask[l];
                MVReg src_b = (m < lanes) ? ba : bb;
                uint32_t sl = (m < lanes) ? m : (uint32_t)(m - lanes);
                MVReg tmp = new_gp(c, eb == 8u ? 8u : 4u);
                MInst ld = minst_make(MI_X64_MOV_RM, eb, loc);
                ld.op_count = 3u; ld.ops[0] = mop_vreg_def(tmp); ld.ops[1] = mop_vreg_use(src_b);
                ld.ops[2] = mop_imm((int64_t)(sl * eb));
                emit(c, ld);
                MInst st = minst_make(MI_X64_MOV_MR, eb, loc);
                st.op_count = 3u; st.ops[0] = mop_vreg_use(tmp); st.ops[1] = mop_vreg_use(bd);
                st.ops[2] = mop_imm((int64_t)(l * eb));
                emit(c, st);
            }
            emit_vec_load_slot(c, dst0, sd, vb, loc);
            return true;
        }

        case PIR_OP_ALLOCA_PTR: {
            uint32_t sid = mfunc_add_slot(c->mf, inst->imm_u64, inst->align, false);
            MInst fa = minst_make(MI_FRAME_ADDR, 8u, loc);
            fa.op_count = 3u; fa.ops[0] = mop_vreg_def(dst0); fa.ops[1] = mop_slot(sid); fa.ops[2] = mop_imm(0);
            emit(c, fa);
            return true;
        }

        case PIR_OP_ALLOCA_DYN: {
            c->mf->has_alloca_dyn = true;
            MVReg sz = mat_op(c, &inst->operands[0], loc);
            MVReg sz64 = new_gp(c, 8u);
            if (inst->operands[0].type.u.scalar == PIR_SCALAR_I32) {
                MInst zx = minst_make(MI_X64_MOV_RR, 4u, loc);
                zx.op_count = 2u; zx.ops[0] = mop_vreg_def(sz64); zx.ops[1] = mop_vreg_use(sz);
                emit(c, zx);
            } else {
                emit_copy(c, sz64, sz, 8u, loc);
            }
            uint64_t al = inst->align > 16u ? (uint64_t)inst->align : 16u;
            MInst ad = minst_make(MI_X64_ADD_RI, 8u, loc);
            ad.op_count = 2u; ad.ops[0] = mop_vreg_defuse(sz64); ad.ops[1] = mop_imm((int64_t)(al - 1u));
            emit(c, ad);
            MInst an = minst_make(MI_X64_AND_RI, 8u, loc);
            an.op_count = 2u; an.ops[0] = mop_vreg_defuse(sz64); an.ops[1] = mop_imm(-(int64_t)al);
            emit(c, an);

            MVReg sp_v = new_gp(c, 8u);
            MInst gsp = minst_make(MI_X64_MOV_RSP, 8u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(sp_v);
            emit(c, gsp);
            MInst sb = minst_make(MI_X64_SUB_RR, 8u, loc);
            sb.op_count = 2u; sb.ops[0] = mop_vreg_defuse(sp_v); sb.ops[1] = mop_vreg_use(sz64);
            emit(c, sb);
            if (al > 16u) {
                MInst an2 = minst_make(MI_X64_AND_RI, 8u, loc);
                an2.op_count = 2u; an2.ops[0] = mop_vreg_defuse(sp_v); an2.ops[1] = mop_imm(-(int64_t)al);
                emit(c, an2);
            }
            MInst ssp = minst_make(MI_X64_MOV_RSP, 8u, loc);
            ssp.aux_id = 1u; ssp.op_count = 1u; ssp.ops[0] = mop_vreg_use(sp_v);
            emit(c, ssp);
            emit_copy(c, dst0, sp_v, 8u, loc);
            return true;
        }

        case PIR_OP_STACKSAVE: {
            c->mf->has_alloca_dyn = true;
            MInst gsp = minst_make(MI_X64_MOV_RSP, 8u, loc);
            gsp.aux_id = 0u; gsp.op_count = 1u; gsp.ops[0] = mop_vreg_def(dst0);
            emit(c, gsp);
            return true;
        }

        case PIR_OP_STACKRESTORE: {
            c->mf->has_alloca_dyn = true;
            MVReg p = mat_op(c, &inst->operands[0], loc);
            MInst ssp = minst_make(MI_X64_MOV_RSP, 8u, loc);
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
                    emit_copy(c, dst0, base, 8u, loc);
                    return true;
                }
                if (off >= INT32_MIN && off <= INT32_MAX) {
                    MInst lea = minst_make(MI_X64_LEA, 8u, loc);
                    lea.op_count = 3u;
                    lea.ops[0] = mop_vreg_def(dst0); lea.ops[1] = mop_vreg_use(base); lea.ops[2] = mop_imm(off);
                    emit(c, lea);
                    return true;
                }
                MVReg oi = new_gp(c, 8u);
                emit_imm_gp(c, oi, (uint64_t)off, 8u, loc);
                MInst lea = minst_make(MI_X64_LEA_IDX, 8u, loc);
                lea.aux_width = 0u; lea.op_count = 4u;
                lea.ops[0] = mop_vreg_def(dst0); lea.ops[1] = mop_vreg_use(base);
                lea.ops[2] = mop_vreg_use(oi);   lea.ops[3] = mop_imm(0);
                emit(c, lea);
                return true;
            }
            MVReg idx = mat_op(c, &inst->operands[1], loc);
            if (inst->ty.u.scalar == PIR_SCALAR_I32) {
                MVReg sx = new_gp(c, 8u);
                MInst e = minst_make(MI_X64_MOVSX, 8u, loc);
                e.aux_width = 4u; e.op_count = 2u; e.ops[0] = mop_vreg_def(sx); e.ops[1] = mop_vreg_use(idx);
                emit(c, e);
                idx = sx;
            }
            MInst lea = minst_make(MI_X64_LEA_IDX, 8u, loc);
            lea.aux_width = 0u; lea.op_count = 4u;
            lea.ops[0] = mop_vreg_def(dst0); lea.ops[1] = mop_vreg_use(base);
            lea.ops[2] = mop_vreg_use(idx);  lea.ops[3] = mop_imm(0);
            emit(c, lea);
            return true;
        }

        case PIR_OP_LOAD: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            bool is_fp = (pir_type_rclass(inst->ty) == MREG_CLASS_FP);
            MVReg base = MVREG_NONE, idx = MVREG_NONE;
            int64_t disp = 0;
            bool has_idx = false;
            uint8_t sh = 0u;
            try_match_mem_addr(c, &inst->operands[0], &base, &disp, &has_idx, &idx, &sh, loc);
            if (has_idx) {
                MInst ld = minst_make(is_fp ? MI_X64_MOVX_RM_IDX : MI_X64_MOV_RM_IDX, mb, loc);
                ld.aux_width = sh; ld.op_count = 4u;
                ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(base);
                ld.ops[2] = mop_vreg_use(idx);  ld.ops[3] = mop_imm(disp);
                emit(c, ld);
            } else {
                MInst ld = minst_make(is_fp ? MI_X64_MOVX_RM : MI_X64_MOV_RM, mb, loc);
                ld.op_count = 3u;
                ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_imm(disp);
                emit(c, ld);
            }
            return true;
        }

        case PIR_OP_STORE: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            bool is_fp = (pir_type_rclass(inst->ty) == MREG_CLASS_FP);
            MVReg val  = mat_op(c, &inst->operands[0], loc);
            MVReg base = MVREG_NONE, idx = MVREG_NONE;
            int64_t disp = 0;
            bool has_idx = false;
            uint8_t sh = 0u;
            try_match_mem_addr(c, &inst->operands[1], &base, &disp, &has_idx, &idx, &sh, loc);
            if (has_idx) {
                MInst st = minst_make(is_fp ? MI_X64_MOVX_MR_IDX : MI_X64_MOV_MR_IDX, mb, loc);
                st.aux_width = sh; st.op_count = 4u;
                st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(base);
                st.ops[2] = mop_vreg_use(idx); st.ops[3] = mop_imm(disp);
                emit(c, st);
            } else {
                MInst st = minst_make(is_fp ? MI_X64_MOVX_MR : MI_X64_MOV_MR, mb, loc);
                st.op_count = 3u;
                st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(base); st.ops[2] = mop_imm(disp);
                emit(c, st);
            }
            return true;
        }

        case PIR_OP_ATOMIC_LOAD: {
            /* x86-64 TSO: aligned mov provides relaxed/acquire/seq_cst load semantics! */
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MInst ld = minst_make(MI_X64_MOV_RM, mb, loc);
            ld.op_count = 3u; ld.ops[0] = mop_vreg_def(dst0); ld.ops[1] = mop_vreg_use(ptr); ld.ops[2] = mop_imm(0);
            emit(c, ld);
            return true;
        }

        case PIR_OP_ATOMIC_STORE: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            MVReg val = mat_op(c, &inst->operands[0], loc);
            MVReg ptr = mat_op(c, &inst->operands[1], loc);
            MInst st = minst_make(MI_X64_MOV_MR, mb, loc);
            st.op_count = 3u; st.ops[0] = mop_vreg_use(val); st.ops[1] = mop_vreg_use(ptr); st.ops[2] = mop_imm(0);
            emit(c, st);
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst mf = minst_make(MI_X64_MFENCE, 0u, loc);
                emit(c, mf);
            }
            return true;
        }

        case PIR_OP_ATOMIC_RMW: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            uint8_t w  = pir_type_vreg_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg val = mat_op(c, &inst->operands[1], loc);
            if (inst->rmw_op == PIR_RMW_ADD || inst->rmw_op == PIR_RMW_SUB ||
                inst->rmw_op == PIR_RMW_XCHG) {
                emit_copy(c, dst0, val, w, loc);
                if (inst->rmw_op == PIR_RMW_SUB) {
                    MInst ng = minst_make(MI_X64_NEG, w, loc);
                    ng.op_count = 1u; ng.ops[0] = mop_vreg_defuse(dst0);
                    emit(c, ng);
                }
                MInst rmw = minst_make(MI_X64_ATOMIC_RMW, mb, loc);
                rmw.aux_id   = (uint32_t)inst->rmw_op;
                rmw.aux_id2  = (uint32_t)inst->order;
                rmw.op_count = 2u;
                rmw.ops[0]   = mop_vreg_defuse(dst0);
                rmw.ops[1]   = mop_vreg_use(ptr);
                emit(c, rmw);
                if (mb < 4u) {
                    MVReg zx = new_gp(c, 4u);
                    MInst z = minst_make(MI_X64_MOVZX, 4u, loc);
                    z.aux_width = mb; z.op_count = 2u; z.ops[0] = mop_vreg_def(zx); z.ops[1] = mop_vreg_use(dst0);
                    emit(c, z);
                    emit_copy(c, dst0, zx, 4u, loc);
                }
            } else {
                /* AND / OR / XOR via lock cmpxchg loop using fixed rax + allocated early-clobber tmp */
                MVReg tmp = new_gp(c, w);
                MInst rmw = minst_make(MI_X64_ATOMIC_RMW, mb, loc);
                rmw.aux_id   = (uint32_t)inst->rmw_op;
                rmw.aux_id2  = (uint32_t)inst->order;
                rmw.op_count = 4u;
                rmw.ops[0]   = mop_vreg_use(ptr);
                rmw.ops[1]   = mop_vreg_use(val);
                rmw.ops[2]   = mop_preg_defuse(X64_RAX);
                rmw.ops[3]   = mop_vreg_def_ec(tmp);
                emit(c, rmw);
                MInst cp = minst_make(MI_COPY, w, loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst0); cp.ops[1] = mop_preg_use(X64_RAX);
                emit(c, cp);
                if (mb < 4u) {
                    MVReg zx = new_gp(c, 4u);
                    MInst z = minst_make(MI_X64_MOVZX, 4u, loc);
                    z.aux_width = mb; z.op_count = 2u; z.ops[0] = mop_vreg_def(zx); z.ops[1] = mop_vreg_use(dst0);
                    emit(c, z);
                    emit_copy(c, dst0, zx, 4u, loc);
                }
            }
            return true;
        }

        case PIR_OP_ATOMIC_CMPXCHG: {
            uint8_t mb = pir_type_mem_bytes(inst->ty);
            uint8_t w  = pir_type_vreg_bytes(inst->ty);
            MVReg ptr = mat_op(c, &inst->operands[0], loc);
            MVReg exp = normalize_int(c, mat_op(c, &inst->operands[1], loc), inst->ty, false, loc);
            MVReg des = mat_op(c, &inst->operands[2], loc);

            MInst me = minst_make(MI_COPY, w, loc);
            me.op_count = 2u; me.ops[0] = mop_preg_def(X64_RAX); me.ops[1] = mop_vreg_use(exp);
            emit(c, me);

            MInst cas = minst_make(MI_X64_ATOMIC_CAS, mb, loc);
            cas.aux_id   = (uint32_t)inst->order;
            cas.aux_id2  = (uint32_t)inst->fail_order;
            cas.op_count = 4u;
            cas.ops[0]   = mop_vreg_def_ec(dst1); /* dst_ok */
            cas.ops[1]   = mop_vreg_use(ptr);
            cas.ops[2]   = mop_vreg_use(des);
            cas.ops[3]   = mop_preg_defuse(X64_RAX);
            emit(c, cas);

            MInst mo = minst_make(MI_COPY, w, loc);
            mo.op_count = 2u; mo.ops[0] = mop_vreg_def(dst0); mo.ops[1] = mop_preg_use(X64_RAX);
            emit(c, mo);
            return true;
        }

        case PIR_OP_FENCE: {
            if (inst->order == PIR_ORDER_SEQ_CST) {
                MInst f = minst_make(MI_X64_MFENCE, 0u, loc);
                emit(c, f);
            }
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
            MInst jmp = minst_make(MI_X64_JMP, 4u, loc);
            jmp.op_count = 1u; jmp.ops[0] = mop_block(dst_mblk);
            emit(c, jmp);
            mfunc_add_edge(c->mf, c->cur_mblk, dst_mblk);
            return true;
        }

        case PIR_OP_BR: {
            uint32_t true_mblk  = prepare_branch_edge(c, &inst->targets[0], loc);
            uint32_t false_mblk = prepare_branch_edge(c, &inst->targets[1], loc);

            bool fused = false;
            if (inst->operands[0].kind == PIR_OPER_VAL) {
                PirValueId cv = inst->operands[0].u.val;
                const PirValueDef *vd = &c->fn->values.data[cv];
                if (vd->def_inst != PIR_INVALID_ID && vd->def_block == cur_pir_blk &&
                    c->inst_fused[vd->def_inst]) {
                    const PirInst *ci = &c->fn->insts.data[vd->def_inst];
                    if (ci->op >= PIR_OP_CMP_EQ && ci->op <= PIR_OP_CMP_GE_U) {
                        emit_scalar_int_cmp_flags(c, ci, loc);
                        MInst jc = minst_make(MI_X64_JCC, 4u, loc);
                        jc.op_count = 2u; jc.ops[0] = mop_cond(int_cmp_to_cond(ci->op)); jc.ops[1] = mop_block(true_mblk);
                        emit(c, jc);
                        fused = true;
                    } else if (ci->op >= PIR_OP_FCMP_OEQ && ci->op <= PIR_OP_FCMP_UGE) {
                        MCond fc_cond;
                        bool swap = false;
                        if (x64_fcmp_single(ci->op, &fc_cond, &swap)) {
                            MVReg fa = mat_op(c, &ci->operands[0], loc);
                            MVReg fb = mat_op(c, &ci->operands[1], loc);
                            uint8_t fw = pir_type_vreg_bytes(ci->ty);
                            MInst fc = minst_make(fw == 8u ? MI_X64_UCOMISD : MI_X64_UCOMISS, fw, loc);
                            fc.op_count = 2u;
                            fc.ops[0] = mop_vreg_use(swap ? fb : fa);
                            fc.ops[1] = mop_vreg_use(swap ? fa : fb);
                            emit(c, fc);
                            MInst jc = minst_make(MI_X64_JCC, 4u, loc);
                            jc.op_count = 2u; jc.ops[0] = mop_cond(fc_cond); jc.ops[1] = mop_block(true_mblk);
                            emit(c, jc);
                            fused = true;
                        }
                    }
                }
            }
            if (!fused) {
                MVReg cond = normalize_int(c, mat_op(c, &inst->operands[0], loc),
                                           pir_type_scalar(PIR_SCALAR_I8), false, loc);
                MInst ts = minst_make(MI_X64_TEST_RR, 4u, loc);
                ts.op_count = 2u; ts.ops[0] = mop_vreg_use(cond); ts.ops[1] = mop_vreg_use(cond);
                emit(c, ts);
                MInst jc = minst_make(MI_X64_JCC, 4u, loc);
                jc.op_count = 2u; jc.ops[0] = mop_cond(MCOND_NE); jc.ops[1] = mop_block(true_mblk);
                emit(c, jc);
            }
            MInst jf = minst_make(MI_X64_JMP, 4u, loc);
            jf.op_count = 1u; jf.ops[0] = mop_block(false_mblk);
            emit(c, jf);
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

                MVReg val64 = val;
                if (w == 4u) {
                    val64 = new_gp(c, 8u);
                    if (min_v < 0) {
                        MInst sx = minst_make(MI_X64_MOVSX, 8u, loc);
                        sx.aux_width = 4u; sx.op_count = 2u;
                        sx.ops[0] = mop_vreg_def(val64); sx.ops[1] = mop_vreg_use(val);
                        emit(c, sx);
                    } else {
                        MInst zx = minst_make(MI_X64_MOV_RR, 4u, loc);
                        zx.op_count = 2u; zx.ops[0] = mop_vreg_def(val64); zx.ops[1] = mop_vreg_use(val);
                        emit(c, zx);
                    }
                }
                MInst sw = minst_make(MI_X64_SWITCH_TBL, 8u, loc);
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
                for (uint32_t i = 0u; i < nc; ++i) {
                    uint64_t cv = (inst->switch_cases[i].is_negative
                                   ? (UINT64_C(0) - inst->switch_cases[i].case_lo)
                                   : inst->switch_cases[i].case_lo) & type_mask;
                    if (fits_simm32(cv, w)) {
                        MInst cmp = minst_make(MI_X64_CMP_RI, w, loc);
                        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(val); cmp.ops[1] = mop_imm(sign_ext_imm(cv, w));
                        emit(c, cmp);
                    } else {
                        MVReg cv_r = new_gp(c, w);
                        emit_imm_gp(c, cv_r, cv, w, loc);
                        MInst cmp = minst_make(MI_X64_CMP_RR, w, loc);
                        cmp.op_count = 2u; cmp.ops[0] = mop_vreg_use(val); cmp.ops[1] = mop_vreg_use(cv_r);
                        emit(c, cmp);
                    }
                    MInst jc = minst_make(MI_X64_JCC, 4u, loc);
                    jc.op_count = 2u; jc.ops[0] = mop_cond(MCOND_EQ); jc.ops[1] = mop_block(case_mblks[i]);
                    emit(c, jc);
                    mfunc_add_edge(c->mf, c->cur_mblk, case_mblks[i]);
                }
                MInst jd = minst_make(MI_X64_JMP, 4u, loc);
                jd.op_count = 1u; jd.ops[0] = mop_block(def_mblk);
                emit(c, jd);
                mfunc_add_edge(c->mf, c->cur_mblk, def_mblk);
            }
            free(case_mblks);
            return true;
        }

        case PIR_OP_RET: {
            if (c->fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG && c->multi_ret_slot != UINT32_MAX) {
                const PirCabiRetInfo *cr = &c->fn->cabi_ret;
                MVReg base = emit_slot_addr(c, c->multi_ret_slot, 0, loc);
                MVReg rvs[4]   = { MVREG_NONE, MVREG_NONE, MVREG_NONE, MVREG_NONE };
                MPReg rregs[4] = { MREG_NONE, MREG_NONE, MREG_NONE, MREG_NONE };
                uint64_t off = 0u;
                uint32_t gp_ri = 0u, fp_ri = 0u;
                for (uint32_t k = 0u; k < cr->reg_count; ++k) {
                    PirType rt = cr->regs[k];
                    bool is_fp = (pir_type_rclass(rt) == MREG_CLASS_FP);
                    uint8_t mb = pir_type_mem_bytes(rt);
                    rvs[k]   = new_vreg_for_type(c, rt);
                    rregs[k] = is_fp ? X64_XMM(fp_ri++) : (gp_ri++ == 0u ? X64_RAX : X64_RDX);
                    MInst ld = minst_make(is_fp ? MI_X64_MOVX_RM : MI_X64_MOV_RM, mb, loc);
                    ld.op_count = 3u;
                    ld.ops[0] = mop_vreg_def(rvs[k]); ld.ops[1] = mop_vreg_use(base); ld.ops[2] = mop_imm((int64_t)off);
                    emit(c, ld);
                    off += mb;
                }
                for (uint32_t k = 0u; k < cr->reg_count; ++k) {
                    MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(cr->regs[k]), loc);
                    cp.op_count = 2u; cp.ops[0] = mop_preg_def(rregs[k]); cp.ops[1] = mop_vreg_use(rvs[k]);
                    emit(c, cp);
                }
                MInst r = minst_make(MI_X64_RET, 8u, loc);
                r.op_count = (uint8_t)cr->reg_count;
                for (uint32_t k = 0u; k < cr->reg_count; ++k) r.ops[k] = mop_preg_use(rregs[k]);
                emit(c, r);
            } else if (c->fn->cabi_ret.kind == PIR_CABI_RET_SRET_RAX && c->sret_ptr_vreg != MVREG_NONE) {
                /* SysV AMD64 §3.2.3: sret callee MUST return hidden pointer in rax! */
                MInst cp = minst_make(MI_COPY, 8u, loc);
                cp.op_count = 2u; cp.ops[0] = mop_preg_def(X64_RAX); cp.ops[1] = mop_vreg_use(c->sret_ptr_vreg);
                emit(c, cp);
                MInst r = minst_make(MI_X64_RET, 8u, loc);
                r.op_count = 1u; r.ops[0] = mop_preg_use(X64_RAX);
                emit(c, r);
            } else if (inst->operand_count == 1u) {
                PirType rty = c->fn->ret_type;
                MVReg rv = mat_op(c, &inst->operands[0], loc);
                MPReg rreg = (pir_type_rclass(rty) == MREG_CLASS_FP) ? X64_XMM(0u) : X64_RAX;
                MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(rty), loc);
                cp.op_count = 2u; cp.ops[0] = mop_preg_def(rreg); cp.ops[1] = mop_vreg_use(rv);
                emit(c, cp);
                MInst r = minst_make(MI_X64_RET, pir_type_vreg_bytes(rty), loc);
                r.op_count = 1u; r.ops[0] = mop_preg_use(rreg);
                emit(c, r);
            } else {
                MInst r = minst_make(MI_X64_RET, 0u, loc);
                emit(c, r);
            }
            return true;
        }

        case PIR_OP_TRAP:
        case PIR_OP_UNREACHABLE: {
            MInst u = minst_make(MI_X64_UD2, 4u, loc);
            emit(c, u);
            return true;
        }

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
                           "x86-64 ISel encountered unlowered HIR opcode %u", (unsigned)inst->op);
            return false;
    }
    return false;
}

/* =========================================================================
 * Pre-Pass: Use Counts & Fusion Marking
 * ========================================================================= */

static void count_uses_in_ops(const PirOperand *ops, uint32_t n, uint32_t *uses) {
    for (uint32_t i = 0u; i < n; ++i) {
        if (ops[i].kind == PIR_OPER_VAL) uses[ops[i].u.val]++;
    }
}

static void analyze_fusion(X64ISelCtx *c, const PirCfg *cfg) {
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

    /* Fold constant-offset ptr_add into load/store [base + disp32] */
    uint32_t *const_pa_ok_uses = (uint32_t *)calloc((size_t)fn->values.len + 1u, sizeof(uint32_t));
    if (!const_pa_ok_uses) abort();
    for (uint32_t ri = 0u; ri < cfg->rpo_len; ++ri) {
        const PirBlock *blk = &fn->blocks.data[cfg->rpo[ri]];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const PirInst *ins = &fn->insts.data[blk->insts.data[j]];
            if (ins->op == PIR_OP_LOAD || ins->op == PIR_OP_STORE) {
                const PirOperand *pop = (ins->op == PIR_OP_LOAD) ? &ins->operands[0] : &ins->operands[1];
                if (pop->kind == PIR_OPER_VAL) {
                    PirValueId pv = pop->u.val;
                    const PirValueDef *vd = &fn->values.data[pv];
                    if (vd->def_inst != PIR_INVALID_ID) {
                        const PirInst *pa = &fn->insts.data[vd->def_inst];
                        if (pa->op == PIR_OP_PTR_ADD &&
                            pa->operands[0].kind == PIR_OPER_VAL &&
                            pa->operands[1].kind == PIR_OPER_IMM_INT) {
                            int64_t off = (pa->ty.u.scalar == PIR_SCALAR_I32)
                                          ? (int64_t)(int32_t)eval_imm_u64(&pa->operands[1])
                                          : (int64_t)eval_imm_u64(&pa->operands[1]);
                            if (off >= INT32_MIN && off <= INT32_MAX) {
                                const_pa_ok_uses[pv]++;
                            }
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

            if (ins->op == PIR_OP_BR && ins->operands[0].kind == PIR_OPER_VAL) {
                PirValueId cv = ins->operands[0].u.val;
                const PirValueDef *vd = &fn->values.data[cv];
                if (vd->def_inst != PIR_INVALID_ID && vd->def_block == bid && c->val_uses[cv] == 1u) {
                    const PirInst *prev = &fn->insts.data[vd->def_inst];
                    if (prev->ty.kind == PIR_TYPE_SCALAR) {
                        MCond dummy;
                        bool dswap;
                        if ((prev->op >= PIR_OP_CMP_EQ && prev->op <= PIR_OP_CMP_GE_U) ||
                            (prev->op >= PIR_OP_FCMP_OEQ && prev->op <= PIR_OP_FCMP_UGE &&
                             x64_fcmp_single(prev->op, &dummy, &dswap))) {
                            c->inst_fused[vd->def_inst] = true;
                        }
                    }
                }
            }

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
                                bool dswap;
                                if ((prev->op >= PIR_OP_CMP_EQ && prev->op <= PIR_OP_CMP_GE_U) ||
                                    (prev->op >= PIR_OP_FCMP_OEQ && prev->op <= PIR_OP_FCMP_UGE &&
                                     x64_fcmp_single(prev->op, &dummy, &dswap))) {
                                    c->inst_fused[vd->def_inst] = true;
                                }
                            }
                        }
                    }
                }
            }

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

            if (ins->op == PIR_OP_LOAD || ins->op == PIR_OP_STORE) {
                const PirOperand *pop = (ins->op == PIR_OP_LOAD) ? &ins->operands[0] : &ins->operands[1];
                if (pop->kind == PIR_OPER_VAL && c->val_uses[pop->u.val] == 1u) {
                    const PirValueDef *vd = &fn->values.data[pop->u.val];
                    if (vd->def_inst != PIR_INVALID_ID && vd->def_block == bid) {
                        const PirInst *pa = &fn->insts.data[vd->def_inst];
                        if (pa->op == PIR_OP_PTR_ADD && pa->operands[0].kind == PIR_OPER_VAL &&
                            pa->operands[1].kind == PIR_OPER_VAL &&
                            pa->ty.kind == PIR_TYPE_SCALAR && pa->ty.u.scalar == PIR_SCALAR_I64) {
                            c->inst_fused[vd->def_inst] = true;
                            PirValueId iv = pa->operands[1].u.val;
                            const PirValueDef *ivd = &fn->values.data[iv];
                            if (ivd->def_inst != PIR_INVALID_ID && ivd->def_block == bid &&
                                c->val_uses[iv] == 1u) {
                                const PirInst *sh = &fn->insts.data[ivd->def_inst];
                                if (sh->op == PIR_OP_SHL && sh->operands[0].kind == PIR_OPER_VAL &&
                                    sh->operands[1].kind == PIR_OPER_IMM_INT) {
                                    uint64_t amt = eval_imm_u64(&sh->operands[1]);
                                    if (amt <= 3u) {
                                        c->inst_fused[ivd->def_inst] = true;
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

bool pir_x64_isel_func(const PirModule *mod, const PirFunction *fn, uint32_t fn_index,
                       const PirTargetSpec *target, MFunc *out_mf, PirDiag *diag) {
    mfunc_init(out_mf, fn, fn_index, target);

    PirCfg cfg;
    pir_cfg_build(&cfg, fn);

    X64ISelCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod             = mod;
    ctx.fn              = fn;
    ctx.target          = target;
    ctx.mf              = out_mf;
    ctx.diag            = diag;
    ctx.multi_ret_slot  = UINT32_MAX;
    ctx.sret_ptr_vreg   = MVREG_NONE;
    ctx.vec_tmp_slot[0] = UINT32_MAX;
    ctx.vec_tmp_slot[1] = UINT32_MAX;
    ctx.vec_tmp_slot[2] = UINT32_MAX;

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

    /* Emit incoming parameter copies at start of entry block (MBlock 0). */
    ctx.cur_mblk = 0u;
    if (fn->param_count > 0u) {
        uint32_t entry_loc = (fn->blocks.len > 0u && fn->blocks.data[0].insts.len > 0u)
                             ? fn->insts.data[fn->blocks.data[0].insts.data[0]].loc_idx : 0u;
        bool has_multi_out = (fn->cabi_ret.kind == PIR_CABI_RET_MULTI_REG);
        uint32_t p_skip    = has_multi_out ? 1u : 0u;
        uint32_t n_in      = fn->param_count - p_skip;

        if (n_in > 0u) {
            PirType   *ptys     = (PirType *)calloc(n_in, sizeof(PirType));
            uint32_t  *paligns  = (uint32_t *)calloc(n_in, sizeof(uint32_t));
            uint64_t  *pbyval_s = (uint64_t *)calloc(n_in, sizeof(uint64_t));
            uint32_t  *pbyval_a = (uint32_t *)calloc(n_in, sizeof(uint32_t));
            bool      *pforce   = (bool *)calloc(n_in, sizeof(bool));
            X64ArgLoc *plocs    = (X64ArgLoc *)calloc(n_in, sizeof(X64ArgLoc));
            if (!ptys || !paligns || !pbyval_s || !pbyval_a || !pforce || !plocs) abort();

            for (uint32_t i = 0u; i < n_in; ++i) {
                const PirParamDecl *pd = &fn->params[i + p_skip];
                ptys[i]     = pd->type;
                paligns[i]  = pd->abi_stack_align;
                pbyval_s[i] = pd->abi_byval_size;
                pbyval_a[i] = pd->abi_byval_align;
                pforce[i]   = pd->abi_force_stack;
            }
            uint64_t in_stack = 0u;
            uint32_t gp_used = 0u, fp_used = 0u;
            plan_call_args_x64(ptys, paligns, pbyval_s, pbyval_a, pforce,
                               n_in, false, n_in, plocs, &in_stack, &gp_used, &fp_used);
            out_mf->va_fixed_gp          = gp_used;
            out_mf->va_fixed_fp          = fp_used;
            out_mf->va_fixed_stack_bytes = in_stack;

            /* Copy register params FIRST before any stack loads. */
            for (uint32_t i = 0u; i < n_in; ++i) {
                PirValueId pv = fn->params[i + p_skip].val_id;
                if (pv == PIR_INVALID_ID || !plocs[i].is_reg) continue;
                MVReg dst = ctx.val_vreg[pv];
                out_mf->vregs.data[dst].hint_preg = plocs[i].preg;
                MInst cp = minst_make(MI_COPY, pir_type_vreg_bytes(ptys[i]), entry_loc);
                cp.op_count = 2u; cp.ops[0] = mop_vreg_def(dst); cp.ops[1] = mop_preg_use(plocs[i].preg);
                emit(&ctx, cp);
            }
            for (uint32_t i = 0u; i < n_in; ++i) {
                PirValueId pv = fn->params[i + p_skip].val_id;
                if (pv == PIR_INVALID_ID || plocs[i].is_reg) continue;
                MVReg dst = ctx.val_vreg[pv];
                if (plocs[i].is_stack_byval) {
                    MInst lea = minst_make(MI_INARG_ADDR, 8u, entry_loc);
                    lea.op_count = 2u; lea.ops[0] = mop_vreg_def(dst); lea.ops[1] = mop_imm(plocs[i].stack_off);
                    emit(&ctx, lea);
                } else {
                    MInst ld = minst_make(MI_LOAD_INARG, plocs[i].mem_bytes, entry_loc);
                    ld.op_count = 2u; ld.ops[0] = mop_vreg_def(dst); ld.ops[1] = mop_imm(plocs[i].stack_off);
                    emit(&ctx, ld);
                }
            }

            if (fn->cabi_ret.kind == PIR_CABI_RET_SRET_RAX && fn->params[0].val_id != PIR_INVALID_ID) {
                ctx.sret_ptr_vreg = ctx.val_vreg[fn->params[0].val_id];
            }

            free(ptys);
            free(paligns);
            free(pbyval_s);
            free(pbyval_a);
            free(pforce);
            free(plocs);
        }

        if (has_multi_out) {
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
                fa.ops[0] = mop_vreg_def(dst0); fa.ops[1] = mop_slot(ctx.multi_ret_slot); fa.ops[2] = mop_imm(0);
                emit(&ctx, fa);
                if (fn->cabi_ret.agg_size < reg_bytes) {
                    MVReg z = new_gp(&ctx, 8u);
                    emit_imm_gp(&ctx, z, 0u, 8u, entry_loc);
                    for (uint64_t zoff = 0u; zoff < reg_bytes; zoff += 8u) {
                        MInst st = minst_make(MI_X64_MOV_MR, 8u, entry_loc);
                        st.op_count = 3u;
                        st.ops[0] = mop_vreg_use(z); st.ops[1] = mop_vreg_use(dst0); st.ops[2] = mop_imm((int64_t)zoff);
                        emit(&ctx, st);
                    }
                }
            }
        }
    }

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
