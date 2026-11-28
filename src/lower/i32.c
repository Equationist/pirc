/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Middle-End i32 Legalization Pass (for 16-bit targets with max_int_bits == 16, e.g. AVR).
 *
 * Splits all i32 SSA values, parameters, returns, multi-reg cabi_ret descriptors,
 * loads/stores, arithmetic, wide multiplies, division/remainder, shifts, comparisons,
 * casts, atomics, and typed globals into pure i16 LIR.
 */

#include "lower/lower.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    PirValueId lo;
    PirValueId hi;
} I32Pair;

typedef struct {
    PirValueId from;
    PirValueId to;
} I32Alias;

static bool is_i32(PirType t) {
    return t.kind == PIR_TYPE_SCALAR && t.u.scalar == PIR_SCALAR_I32;
}

static uint32_t eval_i32_imm(const PirOperand *op) {
    uint64_t v = op->u.imm_int.lo;
    if (op->u.imm_int.is_negative) {
        v = UINT64_C(0) - v;
    }
    return (uint32_t)(v & 0xFFFFFFFFu);
}

static void operand_to_pair16(const PirOperand *op, const I32Pair *map,
                              PirOperand *out_lo, PirOperand *out_hi) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    if (op->kind == PIR_OPER_VAL) {
        *out_lo = pir_op_val(map[op->u.val].lo, t16);
        *out_hi = pir_op_val(map[op->u.val].hi, t16);
    } else {
        uint32_t v = eval_i32_imm(op);
        *out_lo = pir_op_imm_i((uint16_t)(v & 0xFFFFu), false, t16);
        *out_hi = pir_op_imm_i((uint16_t)((v >> 16u) & 0xFFFFu), false, t16);
    }
}

static PirValueId emit_bin(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirOpcode op, PirType ty, uint32_t flags,
                           PirOperand a, PirOperand b, uint32_t ln, uint32_t lc,
                           PirValueId preset) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.flags         = flags;
    ins.operand_count = 2u;
    ins.operands      = pir_alloc_ops(mod, 2u);
    ins.operands[0]   = a;
    ins.operands[1]   = b;
    PirType rty = (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U)
                  ? pir_type_scalar(PIR_SCALAR_I8) : ty;
    ins.results[0] = (preset != PIR_INVALID_ID)
                     ? preset : pir_fn_new_val(mod, fn, "w16", rty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_un(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                          PirOpcode op, PirType ty, PirOperand a, uint32_t ln, uint32_t lc,
                          PirValueId preset) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.operand_count = 1u;
    ins.operands      = pir_alloc_ops(mod, 1u);
    ins.operands[0]   = a;
    ins.results[0]    = (preset != PIR_INVALID_ID)
                        ? preset : pir_fn_new_val(mod, fn, "u16", ty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit_sel(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirType ty, PirOperand cond, PirOperand tv, PirOperand fv,
                           uint32_t ln, uint32_t lc, PirValueId preset) {
    PirInst ins = pir_make_inst(PIR_OP_SELECT, ty, ln, lc);
    ins.operand_count = 3u;
    ins.operands      = pir_alloc_ops(mod, 3u);
    ins.operands[0]   = cond;
    ins.operands[1]   = tv;
    ins.operands[2]   = fv;
    ins.results[0]    = (preset != PIR_INVALID_ID)
                        ? preset : pir_fn_new_val(mod, fn, "s16", ty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirOperand ensure_val16(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                               PirOperand op, uint32_t ln, uint32_t lc) {
    if (op.kind == PIR_OPER_VAL) return op;
    PirValueId v = emit_un(mod, fn, blk, out, PIR_OP_CONST, op.type, op, ln, lc, PIR_INVALID_ID);
    return pir_op_val(v, op.type);
}

static void emit_trap_if16(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                           PirValueId cond_i8, uint32_t ln, uint32_t lc) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirValueId c16 = emit_un(mod, fn, blk, out, PIR_OP_ZEXT, t16, pir_op_val(cond_i8, t8),
                             ln, lc, PIR_INVALID_ID);
    emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, PIR_FLAG_TRAP_U,
             pir_op_val(c16, t16), pir_op_imm_i(0xFFFFu, false, t16),
             ln, lc, PIR_INVALID_ID);
}

static void emit_mul16_wide_u(PirModule *mod, const PirTargetSpec *target,
                              PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                              PirOperand a, PirOperand b, uint32_t ln, uint32_t lc,
                              PirValueId rlo_preset, PirValueId *out_lo, PirValueId *out_hi) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    if (target->caps.has_mul_wide) {
        PirInst mw = pir_make_inst(PIR_OP_MUL_WIDE_U, t16, ln, lc);
        mw.operand_count = 2u; mw.operands = pir_alloc_ops(mod, 2u);
        mw.operands[0] = a; mw.operands[1] = b;
        mw.results[0]  = (rlo_preset != PIR_INVALID_ID)
                         ? rlo_preset : pir_fn_new_val(mod, fn, "mwl", t16, blk, PIR_INVALID_ID, 0u);
        mw.results[1]  = pir_fn_new_val(mod, fn, "mwh", t16, blk, PIR_INVALID_ID, 1u);
        pir_fn_append_inst(fn, out, mw);
        *out_lo = mw.results[0];
        *out_hi = mw.results[1];
        return;
    }
    PirOperand m8 = pir_op_imm_i(0xFFu, false, t16);
    PirOperand s8 = pir_op_imm_i(8u, false, t16);
    PirValueId a0 = emit_bin(mod, fn, blk, out, PIR_OP_AND, t16, 0u, a, m8, ln, lc, PIR_INVALID_ID);
    PirValueId a1 = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, a, s8, ln, lc, PIR_INVALID_ID);
    PirValueId b0 = emit_bin(mod, fn, blk, out, PIR_OP_AND, t16, 0u, b, m8, ln, lc, PIR_INVALID_ID);
    PirValueId b1 = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, b, s8, ln, lc, PIR_INVALID_ID);

    PirValueId p00 = emit_bin(mod, fn, blk, out, PIR_OP_MUL, t16, 0u, pir_op_val(a0, t16), pir_op_val(b0, t16), ln, lc, PIR_INVALID_ID);
    PirValueId p01 = emit_bin(mod, fn, blk, out, PIR_OP_MUL, t16, 0u, pir_op_val(a0, t16), pir_op_val(b1, t16), ln, lc, PIR_INVALID_ID);
    PirValueId p10 = emit_bin(mod, fn, blk, out, PIR_OP_MUL, t16, 0u, pir_op_val(a1, t16), pir_op_val(b0, t16), ln, lc, PIR_INVALID_ID);
    PirValueId p11 = emit_bin(mod, fn, blk, out, PIR_OP_MUL, t16, 0u, pir_op_val(a1, t16), pir_op_val(b1, t16), ln, lc, PIR_INVALID_ID);

    PirValueId p00_hi = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, pir_op_val(p00, t16), s8, ln, lc, PIR_INVALID_ID);
    PirValueId p01_lo = emit_bin(mod, fn, blk, out, PIR_OP_AND, t16, 0u, pir_op_val(p01, t16), m8, ln, lc, PIR_INVALID_ID);
    PirValueId p10_lo = emit_bin(mod, fn, blk, out, PIR_OP_AND, t16, 0u, pir_op_val(p10, t16), m8, ln, lc, PIR_INVALID_ID);
    PirValueId mid0   = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, 0u, pir_op_val(p00_hi, t16), pir_op_val(p01_lo, t16), ln, lc, PIR_INVALID_ID);
    PirValueId mid    = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, 0u, pir_op_val(mid0, t16), pir_op_val(p10_lo, t16), ln, lc, PIR_INVALID_ID);

    PirValueId p00_lo = emit_bin(mod, fn, blk, out, PIR_OP_AND, t16, 0u, pir_op_val(p00, t16), m8, ln, lc, PIR_INVALID_ID);
    PirValueId mid_sh = emit_bin(mod, fn, blk, out, PIR_OP_SHL, t16, PIR_FLAG_MASK, pir_op_val(mid, t16), s8, ln, lc, PIR_INVALID_ID);
    *out_lo = emit_bin(mod, fn, blk, out, PIR_OP_OR, t16, 0u, pir_op_val(p00_lo, t16), pir_op_val(mid_sh, t16), ln, lc, rlo_preset);

    PirValueId p01_hi = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, pir_op_val(p01, t16), s8, ln, lc, PIR_INVALID_ID);
    PirValueId p10_hi = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, pir_op_val(p10, t16), s8, ln, lc, PIR_INVALID_ID);
    PirValueId mid_hi = emit_bin(mod, fn, blk, out, PIR_OP_SHR, t16, PIR_FLAG_MASK, pir_op_val(mid, t16), s8, ln, lc, PIR_INVALID_ID);
    PirValueId h0     = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, 0u, pir_op_val(p11, t16), pir_op_val(p01_hi, t16), ln, lc, PIR_INVALID_ID);
    PirValueId h1     = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, 0u, pir_op_val(h0, t16), pir_op_val(p10_hi, t16), ln, lc, PIR_INVALID_ID);
    *out_hi = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, 0u, pir_op_val(h1, t16), pir_op_val(mid_hi, t16), ln, lc, PIR_INVALID_ID);
}

/* Add a + b (i16) and accumulate carry (0 or 1) into acc_carry (i16). */
static PirValueId add16_with_carry(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                   PirOperand a, PirOperand b, PirValueId *acc_carry,
                                   uint32_t ln, uint32_t lc, PirValueId preset) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirValueId s = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, PIR_FLAG_WRAP, a, b, ln, lc, preset);
    if (acc_carry) {
        PirValueId c  = emit_bin(mod, fn, blk, out, PIR_OP_CMP_LT_U, t16, 0u,
                                 pir_op_val(s, t16), a, ln, lc, PIR_INVALID_ID);
        PirValueId c16 = emit_un(mod, fn, blk, out, PIR_OP_ZEXT, t16,
                                 pir_op_val(c, t8), ln, lc, PIR_INVALID_ID);
        if (*acc_carry == PIR_INVALID_ID) {
            *acc_carry = c16;
        } else {
            *acc_carry = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                                  pir_op_val(*acc_carry, t16), pir_op_val(c16, t16),
                                  ln, lc, PIR_INVALID_ID);
        }
    }
    return s;
}

static void emit_neg32(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                       PirOperand lo, PirOperand hi, uint32_t ln, uint32_t lc,
                       PirValueId *out_lo, PirValueId *out_hi) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);
    PirValueId nlo = emit_bin(mod, fn, blk, out, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                              pir_op_imm_i(0u, false, t16), lo, ln, lc, PIR_INVALID_ID);
    PirValueId bw  = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t16, 0u,
                              lo, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
    PirValueId bw16 = emit_un(mod, fn, blk, out, PIR_OP_ZEXT, t16, pir_op_val(bw, t8), ln, lc, PIR_INVALID_ID);
    PirValueId h0  = emit_bin(mod, fn, blk, out, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                              pir_op_imm_i(0u, false, t16), hi, ln, lc, PIR_INVALID_ID);
    PirValueId nhi = emit_bin(mod, fn, blk, out, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                              pir_op_val(h0, t16), pir_op_val(bw16, t16), ln, lc, PIR_INVALID_ID);
    *out_lo = nlo;
    *out_hi = nhi;
}

static PirValueId emit_mul32_ovf_u(PirModule *mod, const PirTargetSpec *target,
                                   PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                   PirOperand alo, PirOperand ahi, PirOperand blo, PirOperand bhi,
                                   uint32_t ln, uint32_t lc, PirValueId rlo, PirValueId rhi) {
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    PirType t8  = pir_type_scalar(PIR_SCALAR_I8);

    PirValueId m0l = PIR_INVALID_ID, m0h = PIR_INVALID_ID;
    PirValueId m1l = PIR_INVALID_ID, m1h = PIR_INVALID_ID;
    PirValueId m2l = PIR_INVALID_ID, m2h = PIR_INVALID_ID;
    emit_mul16_wide_u(mod, target, fn, blk, out, alo, blo, ln, lc, rlo, &m0l, &m0h);
    emit_mul16_wide_u(mod, target, fn, blk, out, alo, bhi, ln, lc, PIR_INVALID_ID, &m1l, &m1h);
    emit_mul16_wide_u(mod, target, fn, blk, out, ahi, blo, ln, lc, PIR_INVALID_ID, &m2l, &m2h);

    PirValueId mt0 = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                              pir_op_val(m0h, t16), pir_op_val(m1l, t16), ln, lc, PIR_INVALID_ID);
    PirValueId c1  = emit_bin(mod, fn, blk, out, PIR_OP_CMP_LT_U, t16, 0u,
                              pir_op_val(mt0, t16), pir_op_val(m0h, t16), ln, lc, PIR_INVALID_ID);
    PirValueId mhi = emit_bin(mod, fn, blk, out, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                              pir_op_val(mt0, t16), pir_op_val(m2l, t16), ln, lc, rhi);
    PirValueId c2  = emit_bin(mod, fn, blk, out, PIR_OP_CMP_LT_U, t16, 0u,
                              pir_op_val(mhi, t16), pir_op_val(mt0, t16), ln, lc, PIR_INVALID_ID);

    PirValueId ah_nz   = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t16, 0u, ahi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
    PirValueId bh_nz   = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t16, 0u, bhi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
    PirValueId both_hi = emit_bin(mod, fn, blk, out, PIR_OP_AND, t8, 0u, pir_op_val(ah_nz, t8), pir_op_val(bh_nz, t8), ln, lc, PIR_INVALID_ID);
    PirValueId hi_or   = emit_bin(mod, fn, blk, out, PIR_OP_OR, t16, 0u, pir_op_val(m1h, t16), pir_op_val(m2h, t16), ln, lc, PIR_INVALID_ID);
    PirValueId hi_nz   = emit_bin(mod, fn, blk, out, PIR_OP_CMP_NE, t16, 0u, pir_op_val(hi_or, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
    PirValueId cy_or   = emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(c1, t8), pir_op_val(c2, t8), ln, lc, PIR_INVALID_ID);
    PirValueId o1      = emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(both_hi, t8), pir_op_val(hi_nz, t8), ln, lc, PIR_INVALID_ID);
    return emit_bin(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(o1, t8), pir_op_val(cy_or, t8), ln, lc, PIR_INVALID_ID);
}

static void expand_branch_target_i32(PirModule *mod, PirBranchTarget *bt, const I32Pair *map) {
    if (bt->arg_count == 0u) return;
    bool has_any = false;
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (is_i32(bt->args[k].type)) { has_any = true; break; }
    }
    if (!has_any) return;
    PIR_VEC(PirOperand) nargs = {0};
    for (uint32_t k = 0u; k < bt->arg_count; ++k) {
        if (is_i32(bt->args[k].type)) {
            PirOperand lo, hi;
            operand_to_pair16(&bt->args[k], map, &lo, &hi);
            PIR_VEC_PUSH(&nargs, lo);
            PIR_VEC_PUSH(&nargs, hi);
        } else {
            PIR_VEC_PUSH(&nargs, bt->args[k]);
        }
    }
    bt->arg_count = nargs.len;
    bt->args      = pir_alloc_ops(mod, nargs.len);
    memcpy(bt->args, nargs.data, nargs.len * sizeof(PirOperand));
    PIR_VEC_FREE(&nargs);
}

static void expand_cabi_ret_i32(PirCabiRetInfo *cr) {
    if (cr->kind != PIR_CABI_RET_MULTI_REG) return;
    bool has_i32 = false;
    for (uint32_t k = 0u; k < cr->reg_count; ++k) {
        if (is_i32(cr->regs[k])) { has_i32 = true; break; }
    }
    if (!has_i32) return;
    PirType t16 = pir_type_scalar(PIR_SCALAR_I16);
    PirType nregs[4];
    uint32_t nn = 0u;
    for (uint32_t k = 0u; k < cr->reg_count && nn < 4u; ++k) {
        if (is_i32(cr->regs[k])) {
            if (nn + 2u <= 4u) {
                nregs[nn++] = t16;
                nregs[nn++] = t16;
            }
        } else {
            nregs[nn++] = cr->regs[k];
        }
    }
    cr->reg_count = nn;
    for (uint32_t k = 0u; k < nn; ++k) {
        cr->regs[k] = nregs[k];
    }
    if (cr->agg_align > 2u) cr->agg_align = 2u;
}

static PirFuncId ensure_si3_helper(PirModule *mod, const char *name,
                                   uint32_t ln, PirDiag *diag) {
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType t16   = pir_type_scalar(PIR_SCALAR_I16);
    PirType t32   = pir_type_scalar(PIR_SCALAR_I32);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    PirType ptys[5] = { tptr, t16, t16, t16, t16 };

    for (uint32_t i = 0u; i < mod->funcs.len; ++i) {
        if (strcmp(mod->funcs.data[i].name, name) == 0) {
            const PirFunction *f = &mod->funcs.data[i];
            bool ok_base = (f->cc == PIR_CC_C &&
                            !f->is_variadic &&
                            f->linkage != PIR_LINK_INTERNAL);
            bool ok_post = ok_base &&
                           f->param_count == 5u &&
                           pir_type_eq(f->ret_type, tvoid) &&
                           f->cabi_ret.kind == PIR_CABI_RET_MULTI_REG &&
                           f->cabi_ret.reg_count == 2u &&
                           pir_type_eq(f->cabi_ret.regs[0], t16) &&
                           pir_type_eq(f->cabi_ret.regs[1], t16) &&
                           pir_type_eq(f->params[0].type, tptr);
            for (uint32_t k = 1u; ok_post && k < 5u; ++k) {
                if (!pir_type_eq(f->params[k].type, t16)) ok_post = false;
            }
            bool ok_pre = ok_base &&
                          f->param_count == 2u &&
                          pir_type_eq(f->ret_type, t32) &&
                          pir_type_eq(f->params[0].type, t32) &&
                          pir_type_eq(f->params[1].type, t32);
            if (!ok_post && !ok_pre) {
                pir_diag_error(diag, ln, 0u,
                               "incompatible predeclaration of '%s' required by i32 division legalization",
                               name);
                return PIR_INVALID_ID;
            }
            return i;
        }
    }
    PirFuncId id = pir_ensure_verified_extern_func(mod, name, ptys, 5u, tvoid,
                                                   ln, "i32 division legalization", diag);
    if (id == PIR_INVALID_ID) return PIR_INVALID_ID;
    PirFunction *h = &mod->funcs.data[id];
    h->params[1].abi_group_words = 2u;
    h->params[3].abi_group_words = 2u;
    h->cabi_ret.kind      = PIR_CABI_RET_MULTI_REG;
    h->cabi_ret.reg_count = 2u;
    h->cabi_ret.regs[0]   = t16;
    h->cabi_ret.regs[1]   = t16;
    h->cabi_ret.agg_size  = 4u;
    h->cabi_ret.agg_align = 2u;
    return id;
}

static bool legalize_i32_func(PirModule *mod, const PirTargetSpec *target, PirFuncId fi,
                              const bool *fn_ret_i32, const bool *sig_ret_i32,
                              const uint32_t *fn_orig_params, const uint32_t *sig_orig_params,
                              uint32_t orig_funcs, PirDiag *diag) {
    PirFunction *fn = &mod->funcs.data[fi];
    if (!fn->is_defined) return true;

    expand_cabi_ret_i32(&fn->cabi_ret);

    bool ret_is_i32 = fn_ret_i32[fi];
    bool uses_i32 = ret_is_i32;
    if (!uses_i32) {
        for (uint32_t v = 0u; v < fn->values.len; ++v) {
            if (is_i32(fn->values.data[v].type)) { uses_i32 = true; break; }
        }
    }
    if (!uses_i32) {
        for (uint32_t i = 0u; i < fn->insts.len && !uses_i32; ++i) {
            const PirInst *ins = &fn->insts.data[i];
            if (is_i32(ins->ty)) { uses_i32 = true; break; }
            for (uint32_t k = 0u; k < ins->operand_count; ++k) {
                if (is_i32(ins->operands[k].type)) { uses_i32 = true; break; }
            }
            if (uses_i32) break;
            if (ins->op == PIR_OP_CALL && ins->operands[0].u.sym.is_func &&
                ins->operands[0].u.sym.id < orig_funcs &&
                fn_ret_i32[ins->operands[0].u.sym.id]) {
                uses_i32 = true;
                break;
            }
            if (ins->op == PIR_OP_CALL_INDIRECT && ins->sig_id < mod->sigs.len &&
                sig_ret_i32[ins->sig_id]) {
                uses_i32 = true;
                break;
            }
        }
    }
    if (!uses_i32) return true;

    PirType t16   = pir_type_scalar(PIR_SCALAR_I16);
    PirType t8    = pir_type_scalar(PIR_SCALAR_I8);
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    uint32_t def_al2 = (target->arch == PIR_ARCH_AVR) ? 1u : 2u;

    uint32_t orig_vals = fn->values.len;
    I32Pair *map = (I32Pair *)malloc((orig_vals + 1u) * sizeof(I32Pair));
    if (!map) abort();
    for (uint32_t v = 0u; v < orig_vals; ++v) {
        map[v].lo = PIR_INVALID_ID;
        map[v].hi = PIR_INVALID_ID;
        if (is_i32(fn->values.data[v].type)) {
            PirBlockId db = fn->values.data[v].def_block;
            uint32_t   pi = fn->values.data[v].param_index;
            fn->values.data[v].type = t16;
            map[v].lo = v;
            map[v].hi = pir_fn_new_val(mod, fn, "h16", t16, db, PIR_INVALID_ID, pi);
        }
    }

    /* Expand i32 block parameters into (lo: i16, hi: i16) pairs. */
    for (uint32_t b = 0u; b < fn->blocks.len; ++b) {
        PirValueIdVec nparams = {0};
        for (uint32_t k = 0u; k < fn->blocks.data[b].params.len; ++k) {
            PirValueId pv = fn->blocks.data[b].params.data[k];
            PIR_VEC_PUSH(&nparams, pv);
            if (pv < orig_vals && map[pv].hi != PIR_INVALID_ID) {
                PIR_VEC_PUSH(&nparams, map[pv].hi);
            }
        }
        for (uint32_t k = 0u; k < nparams.len; ++k) {
            fn->values.data[nparams.data[k]].param_index = k;
        }
        PIR_VEC_FREE(&fn->blocks.data[b].params);
        fn->blocks.data[b].params = nparams;
    }

    /* Expand i32 function parameters and prepend hidden ptr if returning i32. */
    PirValueId ret_out_val = PIR_INVALID_ID;
    bool has_i32_param = false;
    for (uint32_t i = 0u; i < fn->param_count; ++i) {
        if (is_i32(fn->params[i].type)) { has_i32_param = true; break; }
    }
    if (has_i32_param || ret_is_i32) {
        PIR_VEC(PirParamDecl) nfp = {0};
        if (ret_is_i32) {
            ret_out_val = pir_fn_new_val(mod, fn, "i32_out", tptr, PIR_INVALID_ID, PIR_INVALID_ID, 0u);
            PirParamDecl op;
            memset(&op, 0, sizeof(op));
            op.name     = fn->values.data[ret_out_val].name;
            op.type     = tptr;
            op.abi_type = tvoid;
            op.val_id   = ret_out_val;
            PIR_VEC_PUSH(&nfp, op);

            fn->ret_type = tvoid;
            if (fn->cc == PIR_CC_C) {
                fn->cabi_ret.kind      = PIR_CABI_RET_MULTI_REG;
                fn->cabi_ret.reg_count = 2u;
                fn->cabi_ret.regs[0]   = t16;
                fn->cabi_ret.regs[1]   = t16;
                fn->cabi_ret.agg_size  = 4u;
                fn->cabi_ret.agg_align = 2u;
            }
        }
        uint32_t in_group_rem = 0u;
        for (uint32_t i = 0u; i < fn->param_count; ++i) {
            if (is_i32(fn->params[i].type)) {
                PirValueId lo_id = fn->params[i].val_id;
                PirValueId hi_id = map[lo_id].hi;
                PirParamDecl p0 = fn->params[i];
                PirParamDecl p1 = fn->params[i];
                p0.type = t16;
                p1.type = t16;
                if (in_group_rem > 0u) {
                    p0.abi_group_words = 0u;
                    in_group_rem--;
                } else if (fn->params[i].abi_group_words >= 2u) {
                    uint8_t gw = fn->params[i].abi_group_words;
                    p0.abi_group_words = (uint8_t)(gw * 2u);
                    in_group_rem = (uint32_t)gw - 1u;
                } else if (target->arch == PIR_ARCH_AVR && fn->cc == PIR_CC_C) {
                    p0.abi_group_words = 2u;
                }
                p1.abi_group_words = 0u;
                p1.abi_stack_align = 0u;
                p0.val_id = target->is_big_endian ? hi_id : lo_id;
                p0.name   = fn->values.data[p0.val_id].name;
                p1.val_id = target->is_big_endian ? lo_id : hi_id;
                p1.name   = fn->values.data[p1.val_id].name;
                PIR_VEC_PUSH(&nfp, p0);
                PIR_VEC_PUSH(&nfp, p1);
            } else {
                if (in_group_rem > 0u) in_group_rem--;
                PIR_VEC_PUSH(&nfp, fn->params[i]);
            }
        }
        for (uint32_t k = 0u; k < nfp.len; ++k) {
            if (nfp.data[k].val_id != PIR_INVALID_ID) {
                fn->values.data[nfp.data[k].val_id].param_index = k;
            }
        }
        fn->param_count = nfp.len;
        fn->params = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, nfp.len);
        memcpy(fn->params, nfp.data, nfp.len * sizeof(PirParamDecl));
        PIR_VEC_FREE(&nfp);
    }

    PIR_VEC(I32Alias) aliases = {0};
    PirInstIdVec entry_allocas = {0};

    uint32_t num_blocks = fn->blocks.len;
    for (uint32_t b = 0u; b < num_blocks; ++b) {
        PirInstIdVec new_insts = {0};
        uint32_t old_len = mod->funcs.data[fi].blocks.data[b].insts.len;

        for (uint32_t j = 0u; j < old_len; ++j) {
            PirInstId iid = mod->funcs.data[fi].blocks.data[b].insts.data[j];
            PirInst copy  = mod->funcs.data[fi].insts.data[iid];
            fn = &mod->funcs.data[fi];
            uint32_t ln = copy.src_line, lc = copy.loc_idx;

            /* Rewrite ret %i32 -> store (lo, hi) to ret_out_val + ret void. */
            if (copy.op == PIR_OP_RET && ret_out_val != PIR_INVALID_ID && copy.operand_count == 1u) {
                PirOperand vlo, vhi;
                operand_to_pair16(&copy.operands[0], map, &vlo, &vhi);
                PirInst s0 = pir_make_inst(PIR_OP_STORE, t16, ln, lc);
                s0.align = def_al2; s0.operand_count = 2u; s0.operands = pir_alloc_ops(mod, 2u);
                s0.operands[0] = target->is_big_endian ? vhi : vlo;
                s0.operands[1] = pir_op_val(ret_out_val, tptr);
                pir_fn_append_inst(fn, &new_insts, s0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(ret_out_val, tptr);
                pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "r32p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst s1 = pir_make_inst(PIR_OP_STORE, t16, ln, lc);
                s1.align = def_al2; s1.operand_count = 2u; s1.operands = pir_alloc_ops(mod, 2u);
                s1.operands[0] = target->is_big_endian ? vlo : vhi;
                s1.operands[1] = pir_op_val(pa.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s1);

                PirInst rt = pir_make_inst(PIR_OP_RET, tvoid, ln, lc);
                pir_fn_append_inst(fn, &new_insts, rt);
                continue;
            }

            /* Split i32 arguments and returns at call / call_indirect sites. */
            if (copy.op == PIR_OP_CALL || copy.op == PIR_OP_CALL_INDIRECT) {
                bool call_ret_i32 = false;
                bool call_is_var  = false;
                bool call_is_c    = false;
                uint32_t fixed_n  = 0u;
                if (copy.op == PIR_OP_CALL && copy.operands[0].u.sym.is_func &&
                    copy.operands[0].u.sym.id < orig_funcs) {
                    const PirFunction *cal = &mod->funcs.data[copy.operands[0].u.sym.id];
                    call_ret_i32 = fn_ret_i32[copy.operands[0].u.sym.id];
                    call_is_var  = cal->is_variadic;
                    call_is_c    = (cal->cc == PIR_CC_C);
                    fixed_n      = fn_orig_params[copy.operands[0].u.sym.id];
                } else if (copy.op == PIR_OP_CALL_INDIRECT && copy.sig_id < mod->sigs.len) {
                    const PirSigDef *sig = &mod->sigs.data[copy.sig_id];
                    call_ret_i32 = sig_ret_i32[copy.sig_id];
                    call_is_var  = sig->is_variadic;
                    call_is_c    = (sig->cc == PIR_CC_C);
                    fixed_n      = sig_orig_params[copy.sig_id];
                }
                bool has_i32_arg = false;
                for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                    if (is_i32(copy.operands[k].type)) { has_i32_arg = true; break; }
                }
                if (call_ret_i32 || has_i32_arg) {
                    PirValueId rbuf = PIR_INVALID_ID;
                    if (call_ret_i32) {
                        rbuf = pir_fn_emit_entry_alloca(mod, fn, "c32_out",
                                                        4u, def_al2, ln, lc, &entry_allocas);
                    }
                    PIR_VEC(PirOperand) nargs = {0};
                    PIR_VEC_PUSH(&nargs, copy.operands[0]);
                    if (call_ret_i32) {
                        PIR_VEC_PUSH(&nargs, pir_op_val(rbuf, tptr));
                    }
                    uint32_t in_group_rem = 0u;
                    for (uint32_t k = 1u; k < copy.operand_count; ++k) {
                        if (is_i32(copy.operands[k].type)) {
                            PirOperand lo, hi;
                            operand_to_pair16(&copy.operands[k], map, &lo, &hi);
                            PirOperand first  = target->is_big_endian ? hi : lo;
                            PirOperand second = target->is_big_endian ? lo : hi;
                            first.call_stack_align  = copy.operands[k].call_stack_align;
                            second.call_stack_align = 0u;
                            first.call_force_stack  = copy.operands[k].call_force_stack;
                            second.call_force_stack = copy.operands[k].call_force_stack;
                            if (in_group_rem > 0u) {
                                first.call_group_words = 0u;
                                in_group_rem--;
                            } else if (copy.operands[k].call_group_words >= 2u) {
                                uint8_t gw = copy.operands[k].call_group_words;
                                first.call_group_words = (uint8_t)(gw * 2u);
                                in_group_rem = (uint32_t)gw - 1u;
                            } else if (target->arch == PIR_ARCH_AVR && call_is_c) {
                                first.call_group_words = 2u;
                            }
                            second.call_group_words = 0u;
                            if (call_is_var && (k - 1u) >= fixed_n) {
                                first.has_explicit_type  = true;
                                second.has_explicit_type = true;
                            }
                            PIR_VEC_PUSH(&nargs, first);
                            PIR_VEC_PUSH(&nargs, second);
                        } else {
                            if (in_group_rem > 0u) in_group_rem--;
                            PIR_VEC_PUSH(&nargs, copy.operands[k]);
                        }
                    }
                    PirInst nc = copy;
                    nc.operand_count = nargs.len;
                    nc.operands = pir_alloc_ops(mod, nargs.len);
                    memcpy(nc.operands, nargs.data, nargs.len * sizeof(PirOperand));
                    PIR_VEC_FREE(&nargs);
                    if (call_ret_i32) {
                        nc.ty         = tvoid;
                        nc.results[0] = PIR_INVALID_ID;
                    }
                    pir_fn_append_inst(fn, &new_insts, nc);

                    if (call_ret_i32 && copy.results[0] != PIR_INVALID_ID) {
                        PirValueId rlo = map[copy.results[0]].lo;
                        PirValueId rhi = map[copy.results[0]].hi;
                        PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                        l0.align = def_al2; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                        l0.operands[0] = pir_op_val(rbuf, tptr);
                        l0.results[0]  = target->is_big_endian ? rhi : rlo;
                        pir_fn_append_inst(fn, &new_insts, l0);

                        PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                        pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                        pa.operands[0] = pir_op_val(rbuf, tptr);
                        pa.operands[1] = pir_op_imm_i(2u, false, t16);
                        pa.results[0]  = pir_fn_new_val(mod, fn, "c32p", tptr, b, PIR_INVALID_ID, 0u);
                        pir_fn_append_inst(fn, &new_insts, pa);

                        PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                        l1.align = def_al2; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                        l1.operands[0] = pir_op_val(pa.results[0], tptr);
                        l1.results[0]  = target->is_big_endian ? rlo : rhi;
                        pir_fn_append_inst(fn, &new_insts, l1);
                    }
                    continue;
                }
            }

            /* Expand branch targets carrying i32 arguments. */
            if (copy.op == PIR_OP_JMP || copy.op == PIR_OP_BR || copy.op == PIR_OP_SWITCH) {
                expand_branch_target_i32(mod, &fn->insts.data[iid].targets[0], map);
                if (copy.op == PIR_OP_BR) {
                    expand_branch_target_i32(mod, &fn->insts.data[iid].targets[1], map);
                } else if (copy.op == PIR_OP_SWITCH) {
                    for (uint32_t c = 0u; c < copy.switch_case_count; ++c) {
                        expand_branch_target_i32(mod, &fn->insts.data[iid].switch_cases[c].target, map);
                    }
                }
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            /* Instructions whose operand is i32 but result is non-i32. */
            if (copy.op == PIR_OP_PTR_ADD && is_i32(copy.ty)) {
                PirOperand olo, ohi;
                operand_to_pair16(&copy.operands[1], map, &olo, &ohi);
                (void)ohi;
                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = copy.flags; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = copy.operands[0];
                pa.operands[1] = olo;
                pa.results[0]  = copy.results[0];
                pir_fn_append_inst(fn, &new_insts, pa);
                continue;
            }
            if (copy.op == PIR_OP_ALLOCA_DYN && copy.operand_count == 1u &&
                is_i32(copy.operands[0].type)) {
                PirOperand slo, shi;
                operand_to_pair16(&copy.operands[0], map, &slo, &shi);
                (void)shi;
                PirInst ad = copy;
                ad.operands = pir_alloc_ops(mod, 1u);
                ad.operands[0] = slo;
                pir_fn_append_inst(fn, &new_insts, ad);
                continue;
            }
            if ((copy.op == PIR_OP_MEMCPY || copy.op == PIR_OP_MEMMOVE || copy.op == PIR_OP_MEMSET) &&
                copy.operand_count == 3u && is_i32(copy.operands[2].type)) {
                PirOperand nlo, nhi;
                operand_to_pair16(&copy.operands[2], map, &nlo, &nhi);
                (void)nhi;
                PirInst mm = copy;
                mm.operands = pir_alloc_ops(mod, 3u);
                mm.operands[0] = copy.operands[0];
                mm.operands[1] = copy.operands[1];
                mm.operands[2] = nlo;
                pir_fn_append_inst(fn, &new_insts, mm);
                continue;
            }
            if (copy.op == PIR_OP_INTTOPTR && copy.operand_count == 1u &&
                is_i32(copy.operands[0].type)) {
                PirOperand xlo, xhi;
                operand_to_pair16(&copy.operands[0], map, &xlo, &xhi);
                (void)xhi;
                emit_un(mod, fn, b, &new_insts, PIR_OP_INTTOPTR, tptr,
                        ensure_val16(mod, fn, b, &new_insts, xlo, ln, lc), ln, lc, copy.results[0]);
                continue;
            }
            if (copy.op == PIR_OP_TRUNC && copy.operand_count == 1u &&
                is_i32(copy.operands[0].type)) {
                PirOperand xlo, xhi;
                operand_to_pair16(&copy.operands[0], map, &xlo, &xhi);
                (void)xhi;
                if (copy.ty.u.scalar == PIR_SCALAR_I16) {
                    if (xlo.kind == PIR_OPER_VAL) {
                        I32Alias al = { copy.results[0], xlo.u.val };
                        PIR_VEC_PUSH(&aliases, al);
                    } else {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, xlo, ln, lc, copy.results[0]);
                    }
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_TRUNC, copy.ty,
                            ensure_val16(mod, fn, b, &new_insts, xlo, ln, lc), ln, lc, copy.results[0]);
                }
                continue;
            }

            if (!is_i32(copy.ty)) {
                PIR_VEC_PUSH(&new_insts, iid);
                continue;
            }

            /* === All remaining cases have copy.ty == i32 === */
            PirValueId rlo = (copy.results[0] != PIR_INVALID_ID && copy.results[0] < orig_vals)
                             ? map[copy.results[0]].lo : PIR_INVALID_ID;
            PirValueId rhi = (copy.results[0] != PIR_INVALID_ID && copy.results[0] < orig_vals)
                             ? map[copy.results[0]].hi : PIR_INVALID_ID;

            if (copy.op == PIR_OP_CONST) {
                PirOperand clo, chi;
                operand_to_pair16(&copy.operands[0], map, &clo, &chi);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, clo, ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, chi, ln, lc, rhi);
            } else if (copy.op == PIR_OP_LOAD) {
                uint32_t al2 = (copy.align > 0u && copy.align < def_al2) ? copy.align : def_al2;
                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l0.flags = copy.flags; l0.align = al2;
                l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = copy.operands[0];
                l0.results[0]  = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = copy.operands[0];
                pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "l32p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l1.flags = copy.flags; l1.align = al2;
                l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_STORE) {
                PirOperand vlo, vhi;
                operand_to_pair16(&copy.operands[0], map, &vlo, &vhi);
                uint32_t al2 = (copy.align > 0u && copy.align < def_al2) ? copy.align : def_al2;
                PirInst s0 = pir_make_inst(PIR_OP_STORE, t16, ln, lc);
                s0.flags = copy.flags; s0.align = al2;
                s0.operand_count = 2u; s0.operands = pir_alloc_ops(mod, 2u);
                s0.operands[0] = target->is_big_endian ? vhi : vlo;
                s0.operands[1] = copy.operands[1];
                pir_fn_append_inst(fn, &new_insts, s0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = copy.operands[1];
                pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "s32p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst s1 = pir_make_inst(PIR_OP_STORE, t16, ln, lc);
                s1.flags = copy.flags; s1.align = al2;
                s1.operand_count = 2u; s1.operands = pir_alloc_ops(mod, 2u);
                s1.operands[0] = target->is_big_endian ? vlo : vhi;
                s1.operands[1] = pir_op_val(pa.results[0], tptr);
                pir_fn_append_inst(fn, &new_insts, s1);
            } else if (copy.op == PIR_OP_ADD || copy.op == PIR_OP_SUB) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                if (copy.op == PIR_OP_ADD) {
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP, alo, blo, ln, lc, rlo);
                    PirValueId c0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                             pir_op_val(rlo, t16), alo, ln, lc, PIR_INVALID_ID);
                    PirValueId c16 = emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t16,
                                             pir_op_val(c0, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId h0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                                             ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                             pir_op_val(h0, t16), pir_op_val(c16, t16), ln, lc, rhi);
                    if (copy.flags & PIR_FLAG_TRAP_U) {
                        PirValueId o1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                                 pir_op_val(h0, t16), ahi, ln, lc, PIR_INVALID_ID);
                        PirValueId o2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                                 pir_op_val(rhi, t16), pir_op_val(h0, t16), ln, lc, PIR_INVALID_ID);
                        PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                                 pir_op_val(o1, t8), pir_op_val(o2, t8), ln, lc, PIR_INVALID_ID);
                        emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                    } else if (copy.flags & PIR_FLAG_TRAP_S) {
                        PirValueId x1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t16, 0u,
                                                 pir_op_val(rhi, t16), ahi, ln, lc, PIR_INVALID_ID);
                        PirValueId x2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t16, 0u,
                                                 pir_op_val(rhi, t16), bhi, ln, lc, PIR_INVALID_ID);
                        PirValueId x3 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t16, 0u,
                                                 pir_op_val(x1, t16), pir_op_val(x2, t16), ln, lc, PIR_INVALID_ID);
                        PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u,
                                                 pir_op_val(x3, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                        emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                    }
                } else {
                    PirValueId b0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                             alo, blo, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, alo, blo, ln, lc, rlo);
                    PirValueId b16 = emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t16,
                                             pir_op_val(b0, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId h0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                                             ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                             pir_op_val(h0, t16), pir_op_val(b16, t16), ln, lc, rhi);
                    if (copy.flags & PIR_FLAG_TRAP_U) {
                        PirValueId o1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                                 ahi, bhi, ln, lc, PIR_INVALID_ID);
                        PirValueId o2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u,
                                                 pir_op_val(h0, t16), pir_op_val(b16, t16), ln, lc, PIR_INVALID_ID);
                        PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                                 pir_op_val(o1, t8), pir_op_val(o2, t8), ln, lc, PIR_INVALID_ID);
                        emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                    } else if (copy.flags & PIR_FLAG_TRAP_S) {
                        PirValueId x1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t16, 0u,
                                                 ahi, bhi, ln, lc, PIR_INVALID_ID);
                        PirValueId x2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t16, 0u,
                                                 ahi, pir_op_val(rhi, t16), ln, lc, PIR_INVALID_ID);
                        PirValueId x3 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t16, 0u,
                                                 pir_op_val(x1, t16), pir_op_val(x2, t16), ln, lc, PIR_INVALID_ID);
                        PirValueId ov = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u,
                                                 pir_op_val(x3, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                        emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                    }
                }
            } else if (copy.op == PIR_OP_MUL) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                if (copy.flags & PIR_FLAG_TRAP_U) {
                    PirValueId ov = emit_mul32_ovf_u(mod, target, fn, b, &new_insts,
                                                     alo, ahi, blo, bhi, ln, lc, rlo, rhi);
                    emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                } else if (copy.flags & PIR_FLAG_TRAP_S) {
                    PirValueId na = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u,
                                             ahi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId nb = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u,
                                             bhi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId nalo = PIR_INVALID_ID, nahi = PIR_INVALID_ID;
                    PirValueId nblo = PIR_INVALID_ID, nbhi = PIR_INVALID_ID;
                    emit_neg32(mod, fn, b, &new_insts, alo, ahi, ln, lc, &nalo, &nahi);
                    emit_neg32(mod, fn, b, &new_insts, blo, bhi, ln, lc, &nblo, &nbhi);
                    PirValueId ual = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(na, t8), pir_op_val(nalo, t16), alo, ln, lc, PIR_INVALID_ID);
                    PirValueId uah = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(na, t8), pir_op_val(nahi, t16), ahi, ln, lc, PIR_INVALID_ID);
                    PirValueId ubl = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(nb, t8), pir_op_val(nblo, t16), blo, ln, lc, PIR_INVALID_ID);
                    PirValueId ubh = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(nb, t8), pir_op_val(nbhi, t16), bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId plo = pir_fn_new_val(mod, fn, "pml", t16, b, PIR_INVALID_ID, 0u);
                    PirValueId phi = pir_fn_new_val(mod, fn, "pmh", t16, b, PIR_INVALID_ID, 0u);
                    PirValueId uov = emit_mul32_ovf_u(mod, target, fn, b, &new_insts,
                                                      pir_op_val(ual, t16), pir_op_val(uah, t16),
                                                      pir_op_val(ubl, t16), pir_op_val(ubh, t16),
                                                      ln, lc, plo, phi);
                    PirValueId neg_res = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t8, 0u,
                                                  pir_op_val(na, t8), pir_op_val(nb, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_msb = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u,
                                                 pir_op_val(phi, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId is_min_h = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u,
                                                   pir_op_val(phi, t16), pir_op_imm_i(0x8000u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId is_min_l = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u,
                                                   pir_op_val(plo, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId is_min   = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u,
                                                   pir_op_val(is_min_h, t8), pir_op_val(is_min_l, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId not_min  = emit_bin(mod, fn, b, &new_insts, PIR_OP_XOR, t8, 0u,
                                                   pir_op_val(is_min, t8), pir_op_imm_i(1u, false, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId neg_bad  = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u,
                                                   pir_op_val(hi_msb, t8), pir_op_val(not_min, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId rng_bad  = emit_sel(mod, fn, b, &new_insts, t8, pir_op_val(neg_res, t8),
                                                   pir_op_val(neg_bad, t8), pir_op_val(hi_msb, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId ov       = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                                   pir_op_val(uov, t8), pir_op_val(rng_bad, t8), ln, lc, PIR_INVALID_ID);
                    emit_trap_if16(mod, fn, b, &new_insts, ov, ln, lc);
                    PirValueId npl = PIR_INVALID_ID, nph = PIR_INVALID_ID;
                    emit_neg32(mod, fn, b, &new_insts, pir_op_val(plo, t16), pir_op_val(phi, t16), ln, lc, &npl, &nph);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(neg_res, t8), pir_op_val(npl, t16), pir_op_val(plo, t16), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(neg_res, t8), pir_op_val(nph, t16), pir_op_val(phi, t16), ln, lc, rhi);
                } else {
                    PirValueId m0l = PIR_INVALID_ID, m0h = PIR_INVALID_ID;
                    emit_mul16_wide_u(mod, target, fn, b, &new_insts, alo, blo, ln, lc, rlo, &m0l, &m0h);
                    PirValueId t1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_MUL, t16, PIR_FLAG_WRAP, alo, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId t2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_MUL, t16, PIR_FLAG_WRAP, ahi, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId h0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP, pir_op_val(m0h, t16), pir_op_val(t1, t16), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP, pir_op_val(h0, t16), pir_op_val(t2, t16), ln, lc, rhi);
                }
            } else if (copy.op == PIR_OP_MUL_WIDE_U || copy.op == PIR_OP_MUL_WIDE_S) {
                /* 32x32 -> 64 wide multiply using four 16x16 -> 32 mul_wide_u.i16 ops! */
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                PirValueId r1lo = map[copy.results[1]].lo;
                PirValueId r1hi = map[copy.results[1]].hi;
                bool is_s = (copy.op == PIR_OP_MUL_WIDE_S);

                PirValueId p00l = PIR_INVALID_ID, p00h = PIR_INVALID_ID;
                PirValueId p01l = PIR_INVALID_ID, p01h = PIR_INVALID_ID;
                PirValueId p10l = PIR_INVALID_ID, p10h = PIR_INVALID_ID;
                PirValueId p11l = PIR_INVALID_ID, p11h = PIR_INVALID_ID;
                emit_mul16_wide_u(mod, target, fn, b, &new_insts, alo, blo, ln, lc, rlo, &p00l, &p00h);
                emit_mul16_wide_u(mod, target, fn, b, &new_insts, alo, bhi, ln, lc, PIR_INVALID_ID, &p01l, &p01h);
                emit_mul16_wide_u(mod, target, fn, b, &new_insts, ahi, blo, ln, lc, PIR_INVALID_ID, &p10l, &p10h);
                emit_mul16_wide_u(mod, target, fn, b, &new_insts, ahi, bhi, ln, lc, PIR_INVALID_ID, &p11l, &p11h);

                /* Word 1 = p00h + p01l + p10l (carries -> c_w2) */
                PirValueId c_w2 = PIR_INVALID_ID;
                PirValueId w1a = add16_with_carry(mod, fn, b, &new_insts, pir_op_val(p00h, t16), pir_op_val(p01l, t16), &c_w2, ln, lc, PIR_INVALID_ID);
                add16_with_carry(mod, fn, b, &new_insts, pir_op_val(w1a, t16), pir_op_val(p10l, t16), &c_w2, ln, lc, rhi);

                /* Word 2 = p11l + p01h + p10h + c_w2 (carries -> c_w3) */
                PirValueId c_w3 = PIR_INVALID_ID;
                PirValueId w2a = add16_with_carry(mod, fn, b, &new_insts, pir_op_val(p11l, t16), pir_op_val(p01h, t16), &c_w3, ln, lc, PIR_INVALID_ID);
                PirValueId w2b = add16_with_carry(mod, fn, b, &new_insts, pir_op_val(w2a, t16), pir_op_val(p10h, t16), &c_w3, ln, lc, PIR_INVALID_ID);
                PirValueId u_r1lo = add16_with_carry(mod, fn, b, &new_insts, pir_op_val(w2b, t16), pir_op_val(c_w2, t16), &c_w3, ln, lc,
                                                     is_s ? PIR_INVALID_ID : r1lo);
                /* Word 3 = p11h + c_w3 */
                PirValueId u_r1hi = emit_bin(mod, fn, b, &new_insts, PIR_OP_ADD, t16, PIR_FLAG_WRAP,
                                             pir_op_val(p11h, t16), pir_op_val(c_w3, t16), ln, lc,
                                             is_s ? PIR_INVALID_ID : r1hi);
                if (is_s) {
                    /* signed hi = unsigned hi - (a < 0 ? b : 0) - (b < 0 ? a : 0) */
                    PirValueId a_neg = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u, ahi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId b_neg = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_S, t16, 0u, bhi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId c1l = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(a_neg, t8), blo, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId c1h = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(a_neg, t8), bhi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId c2l = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(b_neg, t8), alo, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId c2h = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(b_neg, t8), ahi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);

                    PirValueId bw1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u, pir_op_val(u_r1lo, t16), pir_op_val(c1l, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId bw1_16 = emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t16, pir_op_val(bw1, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId s1l = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(u_r1lo, t16), pir_op_val(c1l, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId s1h0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(u_r1hi, t16), pir_op_val(c1h, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId s1h = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(s1h0, t16), pir_op_val(bw1_16, t16), ln, lc, PIR_INVALID_ID);

                    PirValueId bw2 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_LT_U, t16, 0u, pir_op_val(s1l, t16), pir_op_val(c2l, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId bw2_16 = emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t16, pir_op_val(bw2, t8), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(s1l, t16), pir_op_val(c2l, t16), ln, lc, r1lo);
                    PirValueId s2h0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(s1h, t16), pir_op_val(c2h, t16), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP, pir_op_val(s2h0, t16), pir_op_val(bw2_16, t16), ln, lc, r1hi);
                }
            } else if (copy.op == PIR_OP_DIV_S || copy.op == PIR_OP_DIV_U ||
                       copy.op == PIR_OP_REM_S || copy.op == PIR_OP_REM_U) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                bool is_s   = (copy.op == PIR_OP_DIV_S || copy.op == PIR_OP_REM_S);
                bool is_rem = (copy.op == PIR_OP_REM_S || copy.op == PIR_OP_REM_U);

                if (!(copy.flags & PIR_FLAG_UNSAFE)) {
                    PirValueId bor = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t16, 0u, blo, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId bz  = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u,
                                              pir_op_val(bor, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    emit_trap_if16(mod, fn, b, &new_insts, bz, ln, lc);
                }

                PirValueId is_min_m1 = PIR_INVALID_ID;
                PirOperand call_blo = blo, call_bhi = bhi;
                if (is_s && !(copy.flags & PIR_FLAG_UNSAFE)) {
                    PirValueId a_ml = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, alo, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId a_mh = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, ahi, pir_op_imm_i(0x8000u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId b_l1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, blo, pir_op_imm_i(0xFFFFu, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId b_h1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, bhi, pir_op_imm_i(0xFFFFu, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId c1   = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(a_ml, t8), pir_op_val(a_mh, t8), ln, lc, PIR_INVALID_ID);
                    PirValueId c2   = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(b_l1, t8), pir_op_val(b_h1, t8), ln, lc, PIR_INVALID_ID);
                    is_min_m1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(c1, t8), pir_op_val(c2, t8), ln, lc, PIR_INVALID_ID);
                    if (copy.flags & PIR_FLAG_WRAP) {
                        PirValueId sbl = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_min_m1, t8),
                                                  pir_op_imm_i(1u, false, t16), blo, ln, lc, PIR_INVALID_ID);
                        PirValueId sbh = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_min_m1, t8),
                                                  pir_op_imm_i(0u, false, t16), bhi, ln, lc, PIR_INVALID_ID);
                        call_blo = pir_op_val(sbl, t16);
                        call_bhi = pir_op_val(sbh, t16);
                    } else {
                        emit_trap_if16(mod, fn, b, &new_insts, is_min_m1, ln, lc);
                    }
                }

                const char *hname = (copy.op == PIR_OP_DIV_S) ? "__divsi3"
                                  : (copy.op == PIR_OP_DIV_U) ? "__udivsi3"
                                  : (copy.op == PIR_OP_REM_S) ? "__modsi3" : "__umodsi3";
                PirFuncId hid = ensure_si3_helper(mod, hname, ln, diag);
                if (hid == PIR_INVALID_ID) {
                    PIR_VEC_FREE(&new_insts);
                    PIR_VEC_FREE(&aliases);
                    PIR_VEC_FREE(&entry_allocas);
                    free(map);
                    return false;
                }
                fn = &mod->funcs.data[fi];

                PirValueId obuf = pir_fn_emit_entry_alloca(mod, fn, "si3_out", 4u, def_al2, ln, lc, &entry_allocas);
                PirInst hc = pir_make_inst(PIR_OP_CALL, tvoid, ln, lc);
                hc.flags = PIR_FLAG_NOUNWIND;
                hc.operand_count = 6u; hc.operands = pir_alloc_ops(mod, 6u);
                hc.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
                hc.operands[1] = pir_op_val(obuf, tptr);
                hc.operands[2] = target->is_big_endian ? ahi : alo;
                hc.operands[3] = target->is_big_endian ? alo : ahi;
                hc.operands[4] = target->is_big_endian ? call_bhi : call_blo;
                hc.operands[5] = target->is_big_endian ? call_blo : call_bhi;
                if (target->arch == PIR_ARCH_AVR) {
                    hc.operands[2].call_group_words = 2u;
                    hc.operands[4].call_group_words = 2u;
                }
                pir_fn_append_inst(fn, &new_insts, hc);

                bool wrap_sel = (is_s && (copy.flags & PIR_FLAG_WRAP) && is_min_m1 != PIR_INVALID_ID);
                PirValueId lres0 = wrap_sel ? pir_fn_new_val(mod, fn, "s3l", t16, b, PIR_INVALID_ID, 0u) : rlo;
                PirValueId lres1 = wrap_sel ? pir_fn_new_val(mod, fn, "s3h", t16, b, PIR_INVALID_ID, 0u) : rhi;

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l0.align = def_al2; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(obuf, tptr);
                l0.results[0]  = target->is_big_endian ? lres1 : lres0;
                pir_fn_append_inst(fn, &new_insts, l0);

                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(obuf, tptr);
                pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "s3p", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);

                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l1.align = def_al2; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? lres0 : lres1;
                pir_fn_append_inst(fn, &new_insts, l1);

                if (wrap_sel) {
                    uint16_t wlo = 0u;
                    uint16_t whi = is_rem ? 0u : 0x8000u;
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_min_m1, t8),
                             pir_op_imm_i(wlo, false, t16), pir_op_val(lres0, t16), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_min_m1, t8),
                             pir_op_imm_i(whi, false, t16), pir_op_val(lres1, t16), ln, lc, rhi);
                }
            } else if (copy.op == PIR_OP_AND || copy.op == PIR_OP_OR || copy.op == PIR_OP_XOR) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                emit_bin(mod, fn, b, &new_insts, copy.op, t16, 0u, alo, blo, ln, lc, rlo);
                emit_bin(mod, fn, b, &new_insts, copy.op, t16, 0u, ahi, bhi, ln, lc, rhi);
            } else if (copy.op == PIR_OP_SHL || copy.op == PIR_OP_SHR || copy.op == PIR_OP_SAR) {
                PirOperand alo, ahi, slo, shi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &slo, &shi);
                if (copy.flags & PIR_FLAG_TRAP) {
                    PirValueId h_nz = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t16, 0u,
                                               shi, pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId l_ge = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_U, t16, 0u,
                                               slo, pir_op_imm_i(32u, false, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId bad  = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                                               pir_op_val(h_nz, t8), pir_op_val(l_ge, t8), ln, lc, PIR_INVALID_ID);
                    emit_trap_if16(mod, fn, b, &new_insts, bad, ln, lc);
                }
                PirValueId s32 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t16, 0u,
                                          slo, pir_op_imm_i(31u, false, t16), ln, lc, PIR_INVALID_ID);
                PirValueId s16 = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t16, 0u,
                                          pir_op_val(s32, t16), pir_op_imm_i(15u, false, t16), ln, lc, PIR_INVALID_ID);
                PirValueId is_z = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u,
                                           pir_op_val(s32, t16), pir_op_imm_i(0u, false, t16), ln, lc, PIR_INVALID_ID);
                PirValueId ge16 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_GE_U, t16, 0u,
                                           pir_op_val(s32, t16), pir_op_imm_i(16u, false, t16), ln, lc, PIR_INVALID_ID);
                PirValueId inv  = emit_bin(mod, fn, b, &new_insts, PIR_OP_SUB, t16, PIR_FLAG_WRAP,
                                           pir_op_imm_i(16u, false, t16), pir_op_val(s16, t16), ln, lc, PIR_INVALID_ID);

                if (copy.op == PIR_OP_SHL) {
                    PirValueId lo_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t16, PIR_FLAG_MASK,
                                               alo, pir_op_val(s16, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t16, PIR_FLAG_MASK,
                                               ahi, pir_op_val(s16, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId cy   = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHR, t16, PIR_FLAG_MASK,
                                               alo, pir_op_val(inv, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_lt = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t16, 0u,
                                                pir_op_val(hi_s, t16), pir_op_val(cy, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId hi_lt_z = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_z, t8),
                                                  ahi, pir_op_val(hi_lt, t16), ln, lc, PIR_INVALID_ID);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(ge16, t8),
                             pir_op_imm_i(0u, false, t16), pir_op_val(lo_s, t16), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(ge16, t8),
                             pir_op_val(lo_s, t16), pir_op_val(hi_lt_z, t16), ln, lc, rhi);
                } else {
                    bool is_ar = (copy.op == PIR_OP_SAR);
                    PirOpcode rsh = is_ar ? PIR_OP_SAR : PIR_OP_SHR;
                    PirValueId hi_s = emit_bin(mod, fn, b, &new_insts, rsh, t16, PIR_FLAG_MASK,
                                               ahi, pir_op_val(s16, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_s = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHR, t16, PIR_FLAG_MASK,
                                               alo, pir_op_val(s16, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId cy   = emit_bin(mod, fn, b, &new_insts, PIR_OP_SHL, t16, PIR_FLAG_MASK,
                                               ahi, pir_op_val(inv, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_lt = emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t16, 0u,
                                                pir_op_val(lo_s, t16), pir_op_val(cy, t16), ln, lc, PIR_INVALID_ID);
                    PirValueId lo_lt_z = emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(is_z, t8),
                                                  alo, pir_op_val(lo_lt, t16), ln, lc, PIR_INVALID_ID);
                    PirOperand hi_fill = pir_op_imm_i(0u, false, t16);
                    if (is_ar) {
                        PirValueId sgn = emit_bin(mod, fn, b, &new_insts, PIR_OP_SAR, t16, PIR_FLAG_MASK,
                                                  ahi, pir_op_imm_i(15u, false, t16), ln, lc, PIR_INVALID_ID);
                        hi_fill = pir_op_val(sgn, t16);
                    }
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(ge16, t8),
                             pir_op_val(hi_s, t16), pir_op_val(lo_lt_z, t16), ln, lc, rlo);
                    emit_sel(mod, fn, b, &new_insts, t16, pir_op_val(ge16, t8),
                             hi_fill, pir_op_val(hi_s, t16), ln, lc, rhi);
                }
            } else if (copy.op >= PIR_OP_CMP_EQ && copy.op <= PIR_OP_CMP_GE_U) {
                PirOperand alo, ahi, blo, bhi;
                operand_to_pair16(&copy.operands[0], map, &alo, &ahi);
                operand_to_pair16(&copy.operands[1], map, &blo, &bhi);
                PirValueId dst = copy.results[0];
                if (copy.op == PIR_OP_CMP_EQ) {
                    PirValueId e0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId e1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u, pir_op_val(e0, t8), pir_op_val(e1, t8), ln, lc, dst);
                } else if (copy.op == PIR_OP_CMP_NE) {
                    PirValueId n0 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t16, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId n1 = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_NE, t16, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u, pir_op_val(n0, t8), pir_op_val(n1, t8), ln, lc, dst);
                } else {
                    PirOpcode hi_op = PIR_OP_CMP_LT_U;
                    PirOpcode lo_op = PIR_OP_CMP_LT_U;
                    if (copy.op == PIR_OP_CMP_LT_S)      { hi_op = PIR_OP_CMP_LT_S; lo_op = PIR_OP_CMP_LT_U; }
                    else if (copy.op == PIR_OP_CMP_LT_U) { hi_op = PIR_OP_CMP_LT_U; lo_op = PIR_OP_CMP_LT_U; }
                    else if (copy.op == PIR_OP_CMP_LE_S) { hi_op = PIR_OP_CMP_LT_S; lo_op = PIR_OP_CMP_LE_U; }
                    else if (copy.op == PIR_OP_CMP_LE_U) { hi_op = PIR_OP_CMP_LT_U; lo_op = PIR_OP_CMP_LE_U; }
                    else if (copy.op == PIR_OP_CMP_GT_S) { hi_op = PIR_OP_CMP_GT_S; lo_op = PIR_OP_CMP_GT_U; }
                    else if (copy.op == PIR_OP_CMP_GT_U) { hi_op = PIR_OP_CMP_GT_U; lo_op = PIR_OP_CMP_GT_U; }
                    else if (copy.op == PIR_OP_CMP_GE_S) { hi_op = PIR_OP_CMP_GT_S; lo_op = PIR_OP_CMP_GE_U; }
                    else if (copy.op == PIR_OP_CMP_GE_U) { hi_op = PIR_OP_CMP_GT_U; lo_op = PIR_OP_CMP_GE_U; }
                    PirValueId hc   = emit_bin(mod, fn, b, &new_insts, hi_op, t16, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId he   = emit_bin(mod, fn, b, &new_insts, PIR_OP_CMP_EQ, t16, 0u, ahi, bhi, ln, lc, PIR_INVALID_ID);
                    PirValueId lc_v = emit_bin(mod, fn, b, &new_insts, lo_op, t16, 0u, alo, blo, ln, lc, PIR_INVALID_ID);
                    PirValueId eq_l = emit_bin(mod, fn, b, &new_insts, PIR_OP_AND, t8, 0u,
                                               pir_op_val(he, t8), pir_op_val(lc_v, t8), ln, lc, PIR_INVALID_ID);
                    emit_bin(mod, fn, b, &new_insts, PIR_OP_OR, t8, 0u,
                             pir_op_val(hc, t8), pir_op_val(eq_l, t8), ln, lc, dst);
                }
            } else if (copy.op == PIR_OP_SELECT) {
                PirOperand tlo, thi, flo, fhi;
                operand_to_pair16(&copy.operands[1], map, &tlo, &thi);
                operand_to_pair16(&copy.operands[2], map, &flo, &fhi);
                emit_sel(mod, fn, b, &new_insts, t16, copy.operands[0], tlo, flo, ln, lc, rlo);
                emit_sel(mod, fn, b, &new_insts, t16, copy.operands[0], thi, fhi, ln, lc, rhi);
            } else if (copy.op == PIR_OP_ZEXT) {
                PirOperand x = copy.operands[0];
                if (x.type.kind == PIR_TYPE_SCALAR && x.type.u.scalar == PIR_SCALAR_I16) {
                    if (x.kind == PIR_OPER_VAL) {
                        I32Alias al = { rlo, x.u.val };
                        PIR_VEC_PUSH(&aliases, al);
                    } else {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, x, ln, lc, rlo);
                    }
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_ZEXT, t16,
                            ensure_val16(mod, fn, b, &new_insts, x, ln, lc), ln, lc, rlo);
                }
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, pir_op_imm_i(0u, false, t16), ln, lc, rhi);
            } else if (copy.op == PIR_OP_SEXT) {
                PirOperand x = copy.operands[0];
                PirOperand lo_op;
                if (x.type.kind == PIR_TYPE_SCALAR && x.type.u.scalar == PIR_SCALAR_I16) {
                    if (x.kind == PIR_OPER_VAL) {
                        I32Alias al = { rlo, x.u.val };
                        PIR_VEC_PUSH(&aliases, al);
                        lo_op = x;
                    } else {
                        emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, x, ln, lc, rlo);
                        lo_op = pir_op_val(rlo, t16);
                    }
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_SEXT, t16,
                            ensure_val16(mod, fn, b, &new_insts, x, ln, lc), ln, lc, rlo);
                    lo_op = pir_op_val(rlo, t16);
                }
                emit_bin(mod, fn, b, &new_insts, PIR_OP_SAR, t16, PIR_FLAG_MASK,
                         lo_op, pir_op_imm_i(15u, false, t16), ln, lc, rhi);
            } else if (copy.op == PIR_OP_PTRTOINT) {
                emit_un(mod, fn, b, &new_insts, PIR_OP_PTRTOINT, t16, copy.operands[0], ln, lc, rlo);
                emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, pir_op_imm_i(0u, false, t16), ln, lc, rhi);
            } else if (copy.op == PIR_OP_ATOMIC_LOAD) {
                PirType ptys[2] = { tptr, tptr };
                PirFuncId hid = pir_ensure_verified_extern_func(mod, "__pir_atomic_load_4", ptys, 2u, tvoid,
                                                                ln, "i32 atomic legalization", diag);
                if (hid == PIR_INVALID_ID) { PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); PIR_VEC_FREE(&entry_allocas); free(map); return false; }
                fn = &mod->funcs.data[fi];
                PirValueId obuf = pir_fn_emit_entry_alloca(mod, fn, "al32", 4u, def_al2, ln, lc, &entry_allocas);
                PirInst hc = pir_make_inst(PIR_OP_CALL, tvoid, ln, lc);
                hc.flags = PIR_FLAG_NOUNWIND; hc.operand_count = 3u; hc.operands = pir_alloc_ops(mod, 3u);
                hc.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
                hc.operands[1] = copy.operands[0];
                hc.operands[2] = pir_op_val(obuf, tptr);
                pir_fn_append_inst(fn, &new_insts, hc);

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l0.align = def_al2; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(obuf, tptr);
                l0.results[0]  = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);
                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(obuf, tptr); pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "alp", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);
                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l1.align = def_al2; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_ATOMIC_STORE) {
                PirOperand vlo, vhi;
                operand_to_pair16(&copy.operands[0], map, &vlo, &vhi);
                PirType ptys[3] = { tptr, t16, t16 };
                PirFuncId hid = pir_ensure_verified_extern_func(mod, "__pir_atomic_store_4", ptys, 3u, tvoid,
                                                                ln, "i32 atomic legalization", diag);
                if (hid == PIR_INVALID_ID) { PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); PIR_VEC_FREE(&entry_allocas); free(map); return false; }
                fn = &mod->funcs.data[fi];
                PirInst hc = pir_make_inst(PIR_OP_CALL, tvoid, ln, lc);
                hc.flags = PIR_FLAG_NOUNWIND; hc.operand_count = 4u; hc.operands = pir_alloc_ops(mod, 4u);
                hc.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
                hc.operands[1] = copy.operands[1];
                hc.operands[2] = vlo; hc.operands[3] = vhi;
                pir_fn_append_inst(fn, &new_insts, hc);
            } else if (copy.op == PIR_OP_ATOMIC_RMW) {
                PirOperand vlo, vhi;
                operand_to_pair16(&copy.operands[1], map, &vlo, &vhi);
                PirType ptys[5] = { tptr, tptr, t16, t16, t16 };
                PirFuncId hid = pir_ensure_verified_extern_func(mod, "__pir_atomic_rmw_4", ptys, 5u, tvoid,
                                                                ln, "i32 atomic legalization", diag);
                if (hid == PIR_INVALID_ID) { PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); PIR_VEC_FREE(&entry_allocas); free(map); return false; }
                fn = &mod->funcs.data[fi];
                PirValueId obuf = pir_fn_emit_entry_alloca(mod, fn, "ar32", 4u, def_al2, ln, lc, &entry_allocas);
                PirInst hc = pir_make_inst(PIR_OP_CALL, tvoid, ln, lc);
                hc.flags = PIR_FLAG_NOUNWIND; hc.operand_count = 6u; hc.operands = pir_alloc_ops(mod, 6u);
                hc.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
                hc.operands[1] = copy.operands[0];
                hc.operands[2] = pir_op_val(obuf, tptr);
                hc.operands[3] = pir_op_imm_i((uint16_t)copy.rmw_op, false, t16);
                hc.operands[4] = vlo; hc.operands[5] = vhi;
                pir_fn_append_inst(fn, &new_insts, hc);

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l0.align = def_al2; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(obuf, tptr);
                l0.results[0]  = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);
                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(obuf, tptr); pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "arp", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);
                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l1.align = def_al2; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_ATOMIC_CMPXCHG) {
                PirOperand elo, ehi, nlo, nhi;
                operand_to_pair16(&copy.operands[1], map, &elo, &ehi);
                operand_to_pair16(&copy.operands[2], map, &nlo, &nhi);
                PirType ptys[6] = { tptr, tptr, t16, t16, t16, t16 };
                PirFuncId hid = pir_ensure_verified_extern_func(mod, "__pir_atomic_cmpxchg_4", ptys, 6u, t8,
                                                                ln, "i32 atomic legalization", diag);
                if (hid == PIR_INVALID_ID) { PIR_VEC_FREE(&new_insts); PIR_VEC_FREE(&aliases); PIR_VEC_FREE(&entry_allocas); free(map); return false; }
                fn = &mod->funcs.data[fi];
                PirValueId obuf = pir_fn_emit_entry_alloca(mod, fn, "ac32", 4u, def_al2, ln, lc, &entry_allocas);
                PirInst hc = pir_make_inst(PIR_OP_CALL, t8, ln, lc);
                hc.flags = PIR_FLAG_NOUNWIND; hc.operand_count = 7u; hc.operands = pir_alloc_ops(mod, 7u);
                hc.operands[0] = pir_op_sym_func(mod->funcs.data[hid].name, hid);
                hc.operands[1] = copy.operands[0];
                hc.operands[2] = pir_op_val(obuf, tptr);
                hc.operands[3] = elo; hc.operands[4] = ehi;
                hc.operands[5] = nlo; hc.operands[6] = nhi;
                hc.results[0]  = copy.results[1]; /* ok: i8 */
                pir_fn_append_inst(fn, &new_insts, hc);

                PirInst l0 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l0.align = def_al2; l0.operand_count = 1u; l0.operands = pir_alloc_ops(mod, 1u);
                l0.operands[0] = pir_op_val(obuf, tptr);
                l0.results[0]  = target->is_big_endian ? rhi : rlo;
                pir_fn_append_inst(fn, &new_insts, l0);
                PirInst pa = pir_make_inst(PIR_OP_PTR_ADD, t16, ln, lc);
                pa.flags = PIR_FLAG_INBOUNDS; pa.operand_count = 2u; pa.operands = pir_alloc_ops(mod, 2u);
                pa.operands[0] = pir_op_val(obuf, tptr); pa.operands[1] = pir_op_imm_i(2u, false, t16);
                pa.results[0]  = pir_fn_new_val(mod, fn, "acp", tptr, b, PIR_INVALID_ID, 0u);
                pir_fn_append_inst(fn, &new_insts, pa);
                PirInst l1 = pir_make_inst(PIR_OP_LOAD, t16, ln, lc);
                l1.align = def_al2; l1.operand_count = 1u; l1.operands = pir_alloc_ops(mod, 1u);
                l1.operands[0] = pir_op_val(pa.results[0], tptr);
                l1.results[0]  = target->is_big_endian ? rlo : rhi;
                pir_fn_append_inst(fn, &new_insts, l1);
            } else if (copy.op == PIR_OP_BITCAST) {
                PirOperand slo, shi;
                operand_to_pair16(&copy.operands[0], map, &slo, &shi);
                if (slo.kind == PIR_OPER_VAL) {
                    I32Alias a0 = { rlo, slo.u.val };
                    PIR_VEC_PUSH(&aliases, a0);
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, slo, ln, lc, rlo);
                }
                if (shi.kind == PIR_OPER_VAL) {
                    I32Alias a1 = { rhi, shi.u.val };
                    PIR_VEC_PUSH(&aliases, a1);
                } else {
                    emit_un(mod, fn, b, &new_insts, PIR_OP_CONST, t16, shi, ln, lc, rhi);
                }
            } else {
                PIR_VEC_PUSH(&new_insts, iid);
            }
        }
        PIR_VEC_FREE(&mod->funcs.data[fi].blocks.data[b].insts);
        mod->funcs.data[fi].blocks.data[b].insts = new_insts;
    }

    fn = &mod->funcs.data[fi];
    pir_fn_prepend_entry_insts(fn, &entry_allocas);
    for (uint32_t i = 0u; i < aliases.len; ++i) {
        PirValueId root = aliases.data[i].to;
        uint32_t steps = 0u;
        for (bool progress = true; progress && steps <= aliases.len; ++steps) {
            progress = false;
            for (uint32_t k = 0u; k < aliases.len; ++k) {
                if (aliases.data[k].from == root) {
                    if (aliases.data[k].to == aliases.data[i].from) { progress = false; break; }
                    root = aliases.data[k].to;
                    progress = true;
                    break;
                }
            }
        }
        if (root != aliases.data[i].from) {
            pir_fn_replace_val_uses(fn, aliases.data[i].from, root);
        }
    }
    PIR_VEC_FREE(&aliases);
    free(map);
    return true;
}

static void split_i32_decl(PirModule *mod, const PirTargetSpec *target, PirCallingConv cc,
                           PirParamDecl **params_io, uint32_t *count_io,
                           PirType *ret_ty_io, PirCabiRetInfo *cabi_ret_io) {
    expand_cabi_ret_i32(cabi_ret_io);
    PirParamDecl *old_p = *params_io;
    uint32_t old_n      = *count_io;
    bool ret_is_i32     = is_i32(*ret_ty_io);
    bool has_any        = ret_is_i32;
    for (uint32_t i = 0u; i < old_n && !has_any; ++i) {
        if (is_i32(old_p[i].type)) { has_any = true; break; }
    }
    if (!has_any) return;
    PirType t16   = pir_type_scalar(PIR_SCALAR_I16);
    PirType tptr  = pir_type_scalar(PIR_SCALAR_PTR);
    PirType tvoid = pir_type_scalar(PIR_SCALAR_VOID);
    PIR_VEC(PirParamDecl) np = {0};
    if (ret_is_i32) {
        PirParamDecl op;
        memset(&op, 0, sizeof(op));
        op.type     = tptr;
        op.abi_type = tvoid;
        op.val_id   = PIR_INVALID_ID;
        PIR_VEC_PUSH(&np, op);
        *ret_ty_io = tvoid;
        if (cc == PIR_CC_C) {
            cabi_ret_io->kind      = PIR_CABI_RET_MULTI_REG;
            cabi_ret_io->reg_count = 2u;
            cabi_ret_io->regs[0]   = t16;
            cabi_ret_io->regs[1]   = t16;
            cabi_ret_io->agg_size  = 4u;
            cabi_ret_io->agg_align = 2u;
        }
    }
    uint32_t in_group_rem = 0u;
    for (uint32_t i = 0u; i < old_n; ++i) {
        if (is_i32(old_p[i].type)) {
            PirParamDecl p0 = old_p[i];
            PirParamDecl p1 = old_p[i];
            p0.type = t16; p1.type = t16;
            if (in_group_rem > 0u) {
                p0.abi_group_words = 0u;
                in_group_rem--;
            } else if (old_p[i].abi_group_words >= 2u) {
                uint8_t gw = old_p[i].abi_group_words;
                p0.abi_group_words = (uint8_t)(gw * 2u);
                in_group_rem = (uint32_t)gw - 1u;
            } else if (target->arch == PIR_ARCH_AVR && cc == PIR_CC_C) {
                p0.abi_group_words = 2u;
            }
            p1.abi_group_words = 0u;
            p1.abi_stack_align = 0u;
            p1.name = NULL; p1.val_id = PIR_INVALID_ID;
            PIR_VEC_PUSH(&np, p0);
            PIR_VEC_PUSH(&np, p1);
        } else {
            if (in_group_rem > 0u) in_group_rem--;
            PIR_VEC_PUSH(&np, old_p[i]);
        }
    }
    *count_io  = np.len;
    *params_io = PIR_ARENA_NEW_ARRAY(&mod->arena, PirParamDecl, np.len);
    memcpy(*params_io, np.data, np.len * sizeof(PirParamDecl));
    PIR_VEC_FREE(&np);
}

bool pir_legalize_i32(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    if (!target || target->caps.max_int_bits == 0u || target->caps.max_int_bits >= 32u) {
        return true;
    }

    uint32_t orig_funcs = mod->funcs.len;
    uint32_t orig_sigs  = mod->sigs.len;
    bool     *fn_ret_i32      = (bool *)calloc(orig_funcs + 1u, sizeof(bool));
    bool     *sig_ret_i32     = (bool *)calloc(orig_sigs + 1u, sizeof(bool));
    uint32_t *fn_orig_params  = (uint32_t *)calloc(orig_funcs + 1u, sizeof(uint32_t));
    uint32_t *sig_orig_params = (uint32_t *)calloc(orig_sigs + 1u, sizeof(uint32_t));
    if (!fn_ret_i32 || !sig_ret_i32 || !fn_orig_params || !sig_orig_params) abort();

    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        fn_ret_i32[fi]     = is_i32(mod->funcs.data[fi].ret_type);
        fn_orig_params[fi] = mod->funcs.data[fi].param_count;
    }
    for (uint32_t si = 0u; si < orig_sigs; ++si) {
        sig_ret_i32[si]     = is_i32(mod->sigs.data[si].ret_type);
        sig_orig_params[si] = mod->sigs.data[si].param_count;
    }

    bool ok = true;
    for (uint32_t fi = 0u; fi < orig_funcs; ++fi) {
        if (mod->funcs.data[fi].is_defined) {
            if (!legalize_i32_func(mod, target, fi, fn_ret_i32, sig_ret_i32,
                                   fn_orig_params, sig_orig_params, orig_funcs, diag)) {
                ok = false;
            }
        } else {
            split_i32_decl(mod, target, mod->funcs.data[fi].cc,
                           &mod->funcs.data[fi].params, &mod->funcs.data[fi].param_count,
                           &mod->funcs.data[fi].ret_type, &mod->funcs.data[fi].cabi_ret);
        }
    }
    if (!ok) {
        free(fn_ret_i32);
        free(sig_ret_i32);
        free(fn_orig_params);
        free(sig_orig_params);
        return false;
    }
    for (uint32_t si = 0u; si < orig_sigs; ++si) {
        split_i32_decl(mod, target, mod->sigs.data[si].cc,
                       &mod->sigs.data[si].params, &mod->sigs.data[si].param_count,
                       &mod->sigs.data[si].ret_type, &mod->sigs.data[si].cabi_ret);
    }

    /* Flatten typed i32 globals into raw 4-byte data items. */
    for (uint32_t gi = 0u; gi < mod->globals.len; ++gi) {
        PirGlobal *g = &mod->globals.data[gi];
        if (!g->has_type || !is_i32(g->type)) continue;
        if (target->arch != PIR_ARCH_AVR && g->align < 4u) g->align = 4u;
        if (g->align == 0u) g->align = 1u;
        if (g->linkage == PIR_LINK_EXTERN) {
            g->has_type    = true;
            g->type        = pir_type_scalar(PIR_SCALAR_I8);
            g->is_zeroinit = false;
            g->is_braced   = false;
            g->item_count  = 0u;
            g->items       = NULL;
            continue;
        }
        g->has_type = false;
        g->type     = pir_type_scalar(PIR_SCALAR_VOID);
        if (g->is_zeroinit || g->item_count == 0u || g->items[0].kind == PIR_GITEM_ZERO) {
            g->is_zeroinit = true;
            g->is_braced   = false;
            g->item_count  = 1u;
            g->items       = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind = PIR_GITEM_ZERO;
            g->items[0].len  = 4u;
        } else if (g->items[0].kind == PIR_GITEM_INT) {
            uint64_t val = g->items[0].int_neg
                           ? (UINT64_C(0) - g->items[0].int_lo)
                           : g->items[0].int_lo;
            uint8_t *buf = PIR_ARENA_NEW_ARRAY(&mod->arena, uint8_t, 4u);
            for (uint32_t k = 0u; k < 4u; ++k) {
                uint32_t sh = target->is_big_endian ? ((3u - k) * 8u) : (k * 8u);
                buf[k] = (uint8_t)((val >> sh) & 0xFFu);
            }
            g->is_braced  = true;
            g->item_count = 1u;
            g->items = PIR_ARENA_NEW_ARRAY(&mod->arena, PirGlobalItem, 1u);
            g->items[0].kind  = PIR_GITEM_BYTES;
            g->items[0].bytes = buf;
            g->items[0].len   = 4u;
        }
    }

    free(fn_ret_i32);
    free(sig_ret_i32);
    free(fn_orig_params);
    free(sig_orig_params);
    return true;
}
