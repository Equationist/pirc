/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "lower/lower.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PirValueId emit2(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                        PirOpcode op, PirType ty, uint32_t flags,
                        PirOperand a, PirOperand b, uint32_t ln, uint32_t lc, PirValueId preset) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.flags         = flags;
    ins.operand_count = 2u;
    ins.operands      = pir_alloc_ops(mod, 2u);
    ins.operands[0]   = a;
    ins.operands[1]   = b;
    PirType rty = (op >= PIR_OP_CMP_EQ && op <= PIR_OP_CMP_GE_U)
                  ? pir_type_scalar(PIR_SCALAR_I8) : ty;
    ins.results[0] = (preset != PIR_INVALID_ID)
                     ? preset : pir_fn_new_val(mod, fn, "lg", rty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

static PirValueId emit1(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                        PirOpcode op, PirType ty, PirOperand a,
                        uint32_t ln, uint32_t lc, PirValueId preset) {
    PirInst ins = pir_make_inst(op, ty, ln, lc);
    ins.operand_count = 1u;
    ins.operands      = pir_alloc_ops(mod, 1u);
    ins.operands[0]   = a;
    ins.results[0]    = (preset != PIR_INVALID_ID)
                        ? preset : pir_fn_new_val(mod, fn, "lg", ty, blk, PIR_INVALID_ID, 0u);
    pir_fn_append_inst(fn, out, ins);
    return ins.results[0];
}

/* Expand 64x64->128 mul_wide into 32-bit partial products. */
static void expand_mul_wide_64(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                               bool is_signed, PirOperand a, PirOperand b,
                               PirValueId rlo, PirValueId rhi, uint32_t ln, uint32_t lc) {
    PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
    PirOperand m32 = pir_op_imm_i(0xFFFFFFFFu, false, t64);
    PirOperand s32 = pir_op_imm_i(32u, false, t64);

    PirValueId a0 = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, a, m32, ln, lc, PIR_INVALID_ID);
    PirValueId a1 = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, a, s32, ln, lc, PIR_INVALID_ID);
    PirValueId b0 = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, b, m32, ln, lc, PIR_INVALID_ID);
    PirValueId b1 = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, b, s32, ln, lc, PIR_INVALID_ID);

    PirValueId p00 = emit2(mod, fn, blk, out, PIR_OP_MUL, t64, 0u, pir_op_val(a0, t64), pir_op_val(b0, t64), ln, lc, PIR_INVALID_ID);
    PirValueId p01 = emit2(mod, fn, blk, out, PIR_OP_MUL, t64, 0u, pir_op_val(a0, t64), pir_op_val(b1, t64), ln, lc, PIR_INVALID_ID);
    PirValueId p10 = emit2(mod, fn, blk, out, PIR_OP_MUL, t64, 0u, pir_op_val(a1, t64), pir_op_val(b0, t64), ln, lc, PIR_INVALID_ID);
    PirValueId p11 = emit2(mod, fn, blk, out, PIR_OP_MUL, t64, 0u, pir_op_val(a1, t64), pir_op_val(b1, t64), ln, lc, PIR_INVALID_ID);

    PirValueId p00_hi = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, pir_op_val(p00, t64), s32, ln, lc, PIR_INVALID_ID);
    PirValueId p01_lo = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, pir_op_val(p01, t64), m32, ln, lc, PIR_INVALID_ID);
    PirValueId p10_lo = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, pir_op_val(p10, t64), m32, ln, lc, PIR_INVALID_ID);
    PirValueId mid0   = emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(p00_hi, t64), pir_op_val(p01_lo, t64), ln, lc, PIR_INVALID_ID);
    PirValueId mid    = emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(mid0, t64), pir_op_val(p10_lo, t64), ln, lc, PIR_INVALID_ID);

    PirValueId p00_lo = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, pir_op_val(p00, t64), m32, ln, lc, PIR_INVALID_ID);
    PirValueId mid_sh = emit2(mod, fn, blk, out, PIR_OP_SHL, t64, PIR_FLAG_MASK, pir_op_val(mid, t64), s32, ln, lc, PIR_INVALID_ID);
    emit2(mod, fn, blk, out, PIR_OP_OR, t64, 0u, pir_op_val(p00_lo, t64), pir_op_val(mid_sh, t64), ln, lc, rlo);

    PirValueId p01_hi = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, pir_op_val(p01, t64), s32, ln, lc, PIR_INVALID_ID);
    PirValueId p10_hi = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, pir_op_val(p10, t64), s32, ln, lc, PIR_INVALID_ID);
    PirValueId mid_hi = emit2(mod, fn, blk, out, PIR_OP_SHR, t64, PIR_FLAG_MASK, pir_op_val(mid, t64), s32, ln, lc, PIR_INVALID_ID);
    PirValueId h0 = emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(p11, t64), pir_op_val(p01_hi, t64), ln, lc, PIR_INVALID_ID);
    PirValueId h1 = emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(h0, t64), pir_op_val(p10_hi, t64), ln, lc, PIR_INVALID_ID);

    if (!is_signed) {
        emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(h1, t64), pir_op_val(mid_hi, t64), ln, lc, rhi);
    } else {
        PirValueId hu  = emit2(mod, fn, blk, out, PIR_OP_ADD, t64, 0u, pir_op_val(h1, t64), pir_op_val(mid_hi, t64), ln, lc, PIR_INVALID_ID);
        PirValueId sa  = emit2(mod, fn, blk, out, PIR_OP_SAR, t64, PIR_FLAG_MASK, a, pir_op_imm_i(63u, false, t64), ln, lc, PIR_INVALID_ID);
        PirValueId sb  = emit2(mod, fn, blk, out, PIR_OP_SAR, t64, PIR_FLAG_MASK, b, pir_op_imm_i(63u, false, t64), ln, lc, PIR_INVALID_ID);
        PirValueId ca  = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, pir_op_val(sa, t64), b, ln, lc, PIR_INVALID_ID);
        PirValueId cb  = emit2(mod, fn, blk, out, PIR_OP_AND, t64, 0u, pir_op_val(sb, t64), a, ln, lc, PIR_INVALID_ID);
        PirValueId hs0 = emit2(mod, fn, blk, out, PIR_OP_SUB, t64, 0u, pir_op_val(hu, t64), pir_op_val(ca, t64), ln, lc, PIR_INVALID_ID);
        emit2(mod, fn, blk, out, PIR_OP_SUB, t64, 0u, pir_op_val(hs0, t64), pir_op_val(cb, t64), ln, lc, rhi);
    }
}

/* SWAR popcnt on any scalar integer width (8, 16, 32, 64). */
static PirValueId emit_popcnt_swar(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                   PirType ty, PirOperand x, uint32_t ln, uint32_t lc, PirValueId preset) {
    unsigned bits = pir_scalar_bits(ty.u.scalar, 8u);
    uint64_t mask = (bits == 64u) ? UINT64_MAX : (((uint64_t)1u << bits) - 1u);

    /* Stage 1: pairs */
    PirValueId s1 = emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, x, pir_op_imm_i(1u, false, ty), ln, lc, PIR_INVALID_ID);
    PirValueId m1 = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(s1, ty), pir_op_imm_i(0x5555555555555555ULL & mask, false, ty), ln, lc, PIR_INVALID_ID);
    PirValueId r1 = emit2(mod, fn, blk, out, PIR_OP_SUB, ty, 0u, x, pir_op_val(m1, ty), ln, lc, PIR_INVALID_ID);

    /* Stage 2: nibbles */
    PirOperand m33 = pir_op_imm_i(0x3333333333333333ULL & mask, false, ty);
    PirValueId a2  = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(r1, ty), m33, ln, lc, PIR_INVALID_ID);
    PirValueId s2  = emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, pir_op_val(r1, ty), pir_op_imm_i(2u, false, ty), ln, lc, PIR_INVALID_ID);
    PirValueId b2  = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(s2, ty), m33, ln, lc, PIR_INVALID_ID);
    PirValueId r2  = emit2(mod, fn, blk, out, PIR_OP_ADD, ty, 0u, pir_op_val(a2, ty), pir_op_val(b2, ty), ln, lc, PIR_INVALID_ID);

    /* Stage 3: bytes */
    PirValueId s3  = emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, pir_op_val(r2, ty), pir_op_imm_i(4u, false, ty), ln, lc, PIR_INVALID_ID);
    PirValueId c3  = emit2(mod, fn, blk, out, PIR_OP_ADD, ty, 0u, pir_op_val(r2, ty), pir_op_val(s3, ty), ln, lc, PIR_INVALID_ID);
    PirValueId r3  = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(c3, ty), pir_op_imm_i(0x0F0F0F0F0F0F0F0FULL & mask, false, ty), ln, lc, bits == 8u ? preset : PIR_INVALID_ID);
    if (bits == 8u) return r3;

    /* Horizontal byte sum via multiply + shift */
    PirValueId mul = emit2(mod, fn, blk, out, PIR_OP_MUL, ty, 0u, pir_op_val(r3, ty), pir_op_imm_i(0x0101010101010101ULL & mask, false, ty), ln, lc, PIR_INVALID_ID);
    return emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, pir_op_val(mul, ty), pir_op_imm_i(bits - 8u, false, ty), ln, lc, preset);
}

static void expand_bitop(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                         const PirInst *inst) {
    PirType ty = inst->ty;
    unsigned bits = pir_scalar_bits(ty.u.scalar, 8u);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;
    PirOperand x = inst->operands[0];
    PirValueId res = inst->results[0];

    if (inst->op == PIR_OP_POPCNT) {
        emit_popcnt_swar(mod, fn, blk, out, ty, x, ln, lc, res);
    } else if (inst->op == PIR_OP_CLZ) {
        /* Smear highest set bit rightward, then return bits - popcnt(smeared). */
        PirOperand cur = x;
        for (unsigned sh = 1u; sh < bits; sh <<= 1u) {
            PirValueId s = emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, cur, pir_op_imm_i(sh, false, ty), ln, lc, PIR_INVALID_ID);
            PirValueId o = emit2(mod, fn, blk, out, PIR_OP_OR, ty, 0u, cur, pir_op_val(s, ty), ln, lc, PIR_INVALID_ID);
            cur = pir_op_val(o, ty);
        }
        PirValueId pc = emit_popcnt_swar(mod, fn, blk, out, ty, cur, ln, lc, PIR_INVALID_ID);
        emit2(mod, fn, blk, out, PIR_OP_SUB, ty, 0u, pir_op_imm_i(bits, false, ty), pir_op_val(pc, ty), ln, lc, res);
    } else if (inst->op == PIR_OP_CTZ) {
        /* popcnt((~x) & (x - 1)): handles x == 0 -> bits automatically! */
        uint64_t all1 = (bits == 64u) ? UINT64_MAX : (((uint64_t)1u << bits) - 1u);
        PirValueId nx = emit2(mod, fn, blk, out, PIR_OP_XOR, ty, 0u, x, pir_op_imm_i(all1, false, ty), ln, lc, PIR_INVALID_ID);
        PirValueId xm = emit2(mod, fn, blk, out, PIR_OP_SUB, ty, 0u, x, pir_op_imm_i(1u, false, ty), ln, lc, PIR_INVALID_ID);
        PirValueId m  = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(nx, ty), pir_op_val(xm, ty), ln, lc, PIR_INVALID_ID);
        emit_popcnt_swar(mod, fn, blk, out, ty, pir_op_val(m, ty), ln, lc, res);
    } else { /* PIR_OP_BSWAP */
        unsigned nbytes = bits / 8u;
        PirValueId acc = PIR_INVALID_ID;
        for (unsigned i = 0u; i < nbytes; ++i) {
            unsigned src_sh = i * 8u;
            unsigned dst_sh = (nbytes - 1u - i) * 8u;
            PirOperand b_op = x;
            if (src_sh > 0u) {
                PirValueId s = emit2(mod, fn, blk, out, PIR_OP_SHR, ty, PIR_FLAG_MASK, b_op, pir_op_imm_i(src_sh, false, ty), ln, lc, PIR_INVALID_ID);
                b_op = pir_op_val(s, ty);
            }
            PirValueId masked = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, b_op, pir_op_imm_i(0xFFu, false, ty), ln, lc, PIR_INVALID_ID);
            PirOperand placed = pir_op_val(masked, ty);
            if (dst_sh > 0u) {
                PirValueId l = emit2(mod, fn, blk, out, PIR_OP_SHL, ty, PIR_FLAG_MASK, placed, pir_op_imm_i(dst_sh, false, ty), ln, lc, PIR_INVALID_ID);
                placed = pir_op_val(l, ty);
            }
            if (i == 0u) {
                acc = placed.u.val;
            } else {
                acc = emit2(mod, fn, blk, out, PIR_OP_OR, ty, 0u, pir_op_val(acc, ty), placed,
                            ln, lc, (i + 1u == nbytes) ? res : PIR_INVALID_ID);
            }
        }
    }
}

static PirOperand ensure_val(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                             PirOperand op, uint32_t ln, uint32_t lc) {
    if (op.kind == PIR_OPER_VAL) return op;
    PirValueId v = emit1(mod, fn, blk, out, PIR_OP_CONST, op.type, op, ln, lc, PIR_INVALID_ID);
    return pir_op_val(v, op.type);
}

static void expand_ovf_or_carry(PirModule *mod, PirFunction *fn, PirBlockId blk, PirInstIdVec *out,
                                const PirInst *inst) {
    PirType ty = inst->ty;
    PirType t8 = pir_type_scalar(PIR_SCALAR_I8);
    unsigned bits = pir_scalar_bits(ty.u.scalar, 8u);
    uint32_t ln = inst->src_line, lc = inst->loc_idx;
    PirOperand a = inst->operands[0], b = inst->operands[1];
    PirValueId r0 = inst->results[0], r1 = inst->results[1];

    if (inst->op == PIR_OP_ADD_OVF_U) {
        emit2(mod, fn, blk, out, PIR_OP_ADD, ty, PIR_FLAG_WRAP, a, b, ln, lc, r0);
        emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, pir_op_val(r0, ty), a, ln, lc, r1);
    } else if (inst->op == PIR_OP_SUB_OVF_U) {
        emit2(mod, fn, blk, out, PIR_OP_SUB, ty, PIR_FLAG_WRAP, a, b, ln, lc, r0);
        emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, a, b, ln, lc, r1);
    } else if (inst->op == PIR_OP_ADD_OVF_S) {
        emit2(mod, fn, blk, out, PIR_OP_ADD, ty, PIR_FLAG_WRAP, a, b, ln, lc, r0);
        PirValueId t1 = emit2(mod, fn, blk, out, PIR_OP_XOR, ty, 0u, pir_op_val(r0, ty), a, ln, lc, PIR_INVALID_ID);
        PirValueId t2 = emit2(mod, fn, blk, out, PIR_OP_XOR, ty, 0u, pir_op_val(r0, ty), b, ln, lc, PIR_INVALID_ID);
        PirValueId t3 = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(t1, ty), pir_op_val(t2, ty), ln, lc, PIR_INVALID_ID);
        emit2(mod, fn, blk, out, PIR_OP_CMP_LT_S, ty, 0u, pir_op_val(t3, ty), pir_op_imm_i(0u, false, ty), ln, lc, r1);
    } else if (inst->op == PIR_OP_SUB_OVF_S) {
        emit2(mod, fn, blk, out, PIR_OP_SUB, ty, PIR_FLAG_WRAP, a, b, ln, lc, r0);
        PirValueId t1 = emit2(mod, fn, blk, out, PIR_OP_XOR, ty, 0u, a, b, ln, lc, PIR_INVALID_ID);
        PirValueId t2 = emit2(mod, fn, blk, out, PIR_OP_XOR, ty, 0u, a, pir_op_val(r0, ty), ln, lc, PIR_INVALID_ID);
        PirValueId t3 = emit2(mod, fn, blk, out, PIR_OP_AND, ty, 0u, pir_op_val(t1, ty), pir_op_val(t2, ty), ln, lc, PIR_INVALID_ID);
        emit2(mod, fn, blk, out, PIR_OP_CMP_LT_S, ty, 0u, pir_op_val(t3, ty), pir_op_imm_i(0u, false, ty), ln, lc, r1);
    } else if (inst->op == PIR_OP_MUL_OVF_U || inst->op == PIR_OP_MUL_OVF_S) {
        bool is_s = (inst->op == PIR_OP_MUL_OVF_S);
        if (bits < 64u) {
            PirType wty = pir_type_scalar(bits == 8u ? PIR_SCALAR_I16 : (bits == 16u ? PIR_SCALAR_I32 : PIR_SCALAR_I64));
            PirOpcode ext = is_s ? PIR_OP_SEXT : PIR_OP_ZEXT;
            PirOperand va = ensure_val(mod, fn, blk, out, a, ln, lc);
            PirOperand vb = ensure_val(mod, fn, blk, out, b, ln, lc);
            PirValueId wa = emit1(mod, fn, blk, out, ext, wty, va, ln, lc, PIR_INVALID_ID);
            PirValueId wb = emit1(mod, fn, blk, out, ext, wty, vb, ln, lc, PIR_INVALID_ID);
            PirValueId wp = emit2(mod, fn, blk, out, PIR_OP_MUL, wty, 0u, pir_op_val(wa, wty), pir_op_val(wb, wty), ln, lc, PIR_INVALID_ID);
            emit1(mod, fn, blk, out, PIR_OP_TRUNC, ty, pir_op_val(wp, wty), ln, lc, r0);
            if (is_s) {
                PirValueId back = emit1(mod, fn, blk, out, PIR_OP_SEXT, wty, pir_op_val(r0, ty), ln, lc, PIR_INVALID_ID);
                emit2(mod, fn, blk, out, PIR_OP_CMP_NE, wty, 0u, pir_op_val(wp, wty), pir_op_val(back, wty), ln, lc, r1);
            } else {
                PirValueId hi = emit2(mod, fn, blk, out, PIR_OP_SHR, wty, PIR_FLAG_MASK, pir_op_val(wp, wty), pir_op_imm_i(bits, false, wty), ln, lc, PIR_INVALID_ID);
                emit2(mod, fn, blk, out, PIR_OP_CMP_NE, wty, 0u, pir_op_val(hi, wty), pir_op_imm_i(0u, false, wty), ln, lc, r1);
            }
        } else {
            PirValueId hi = pir_fn_new_val(mod, fn, "mhi", ty, blk, PIR_INVALID_ID, 0u);
            expand_mul_wide_64(mod, fn, blk, out, is_s, a, b, r0, hi, ln, lc);
            if (is_s) {
                PirValueId sgn = emit2(mod, fn, blk, out, PIR_OP_SAR, ty, PIR_FLAG_MASK, pir_op_val(r0, ty), pir_op_imm_i(63u, false, ty), ln, lc, PIR_INVALID_ID);
                emit2(mod, fn, blk, out, PIR_OP_CMP_NE, ty, 0u, pir_op_val(hi, ty), pir_op_val(sgn, ty), ln, lc, r1);
            } else {
                emit2(mod, fn, blk, out, PIR_OP_CMP_NE, ty, 0u, pir_op_val(hi, ty), pir_op_imm_i(0u, false, ty), ln, lc, r1);
            }
        }
    } else if (inst->op == PIR_OP_ADD_CARRY || inst->op == PIR_OP_SUB_BORROW) {
        PirValueId c_bit = emit2(mod, fn, blk, out, PIR_OP_AND, t8, 0u, inst->operands[2], pir_op_imm_i(1u, false, t8), ln, lc, PIR_INVALID_ID);
        PirOperand c_ext = (bits == 8u) ? pir_op_val(c_bit, t8)
                                        : pir_op_val(emit1(mod, fn, blk, out, PIR_OP_ZEXT, ty, pir_op_val(c_bit, t8), ln, lc, PIR_INVALID_ID), ty);
        if (inst->op == PIR_OP_ADD_CARRY) {
            PirValueId s1 = emit2(mod, fn, blk, out, PIR_OP_ADD, ty, PIR_FLAG_WRAP, a, b, ln, lc, PIR_INVALID_ID);
            PirValueId o1 = emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, pir_op_val(s1, ty), a, ln, lc, PIR_INVALID_ID);
            emit2(mod, fn, blk, out, PIR_OP_ADD, ty, PIR_FLAG_WRAP, pir_op_val(s1, ty), c_ext, ln, lc, r0);
            PirValueId o2 = emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, pir_op_val(r0, ty), pir_op_val(s1, ty), ln, lc, PIR_INVALID_ID);
            emit2(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(o1, t8), pir_op_val(o2, t8), ln, lc, r1);
        } else {
            PirValueId o1 = emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, a, b, ln, lc, PIR_INVALID_ID);
            PirValueId d1 = emit2(mod, fn, blk, out, PIR_OP_SUB, ty, PIR_FLAG_WRAP, a, b, ln, lc, PIR_INVALID_ID);
            PirValueId o2 = emit2(mod, fn, blk, out, PIR_OP_CMP_LT_U, ty, 0u, pir_op_val(d1, ty), c_ext, ln, lc, PIR_INVALID_ID);
            emit2(mod, fn, blk, out, PIR_OP_SUB, ty, PIR_FLAG_WRAP, pir_op_val(d1, ty), c_ext, ln, lc, r0);
            emit2(mod, fn, blk, out, PIR_OP_OR, t8, 0u, pir_op_val(o1, t8), pir_op_val(o2, t8), ln, lc, r1);
        }
    }
}

/* =========================================================================
 * Switch -> Balanced Binary Search Tree Lowering
 * ========================================================================= */

typedef struct {
    int64_t       key;
    PirSwitchCase sc;
} SortedCase;

static int cmp_sorted_case(const void *a, const void *b) {
    int64_t ka = ((const SortedCase *)a)->key;
    int64_t kb = ((const SortedCase *)b)->key;
    return (ka < kb) ? -1 : (ka > kb ? 1 : 0);
}

static PirBlockId new_switch_block(PirModule *mod, PirFunction *fn, uint32_t line) {
    char buf[48];
    PirBlockId bid = fn->blocks.len;
    snprintf(buf, sizeof(buf), "sw_%u", bid);
    PirBlock blk;
    memset(&blk, 0, sizeof(blk));
    blk.label    = pir_arena_strdup(&mod->arena, buf);
    blk.src_line = line;
    PIR_VEC_PUSH(&fn->blocks, blk);
    return bid;
}

static void emit_switch_node(PirModule *mod, PirFunction *fn, PirBlockId blk_id,
                             PirInstIdVec *cur_out, PirType ty, PirOperand val,
                             const SortedCase *cases, int lo, int hi,
                             PirBranchTarget def_bt, uint32_t ln, uint32_t lc) {
    PirInstIdVec *out = cur_out ? cur_out : &fn->blocks.data[blk_id].insts;
    if (lo > hi) {
        PirInst j = pir_make_inst(PIR_OP_JMP, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
        j.targets[0] = def_bt;
        pir_fn_append_inst(fn, out, j);
        return;
    }
    int mid = lo + (hi - lo) / 2;
    PirOperand key_op = pir_op_imm_i(cases[mid].sc.case_lo, cases[mid].sc.is_negative, ty);
    PirValueId eq = emit2(mod, fn, blk_id, out, PIR_OP_CMP_EQ, ty, 0u, val, key_op, ln, lc, PIR_INVALID_ID);
    if (lo == hi) {
        PirInst br = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
        br.operand_count = 1u; br.operands = pir_alloc_ops(mod, 1u);
        br.operands[0] = pir_op_val(eq, pir_type_scalar(PIR_SCALAR_I8));
        br.targets[0]  = cases[mid].sc.target;
        br.targets[1]  = def_bt;
        pir_fn_append_inst(fn, out, br);
        return;
    }

    PirBlockId ne_blk = new_switch_block(mod, fn, ln);
    /* Re-fetch out if cur_out == NULL because new_switch_block may realloc fn->blocks.data! */
    out = cur_out ? cur_out : &fn->blocks.data[blk_id].insts;
    PirInst br_eq = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
    br_eq.operand_count = 1u; br_eq.operands = pir_alloc_ops(mod, 1u);
    br_eq.operands[0] = pir_op_val(eq, pir_type_scalar(PIR_SCALAR_I8));
    br_eq.targets[0]  = cases[mid].sc.target;
    br_eq.targets[1].target = ne_blk;
    pir_fn_append_inst(fn, out, br_eq);

    PirBlockId lo_blk = new_switch_block(mod, fn, ln);
    PirBlockId hi_blk = new_switch_block(mod, fn, ln);
    PirValueId lt = emit2(mod, fn, ne_blk, &fn->blocks.data[ne_blk].insts,
                          PIR_OP_CMP_LT_S, ty, 0u, val, key_op, ln, lc, PIR_INVALID_ID);
    PirInst br_lt = pir_make_inst(PIR_OP_BR, pir_type_scalar(PIR_SCALAR_VOID), ln, lc);
    br_lt.operand_count = 1u; br_lt.operands = pir_alloc_ops(mod, 1u);
    br_lt.operands[0] = pir_op_val(lt, pir_type_scalar(PIR_SCALAR_I8));
    br_lt.targets[0].target = lo_blk;
    br_lt.targets[1].target = hi_blk;
    pir_fn_append_inst(fn, &fn->blocks.data[ne_blk].insts, br_lt);

    emit_switch_node(mod, fn, lo_blk, NULL, ty, val, cases, lo, mid - 1, def_bt, ln, lc);
    emit_switch_node(mod, fn, hi_blk, NULL, ty, val, cases, mid + 1, hi, def_bt, ln, lc);
}

bool pir_legalize_optional_ops(PirModule *mod, const PirTargetSpec *target, PirDiag *diag) {
    (void)diag;
    const PirBackendCaps *c = &target->caps;
    if (c->has_mul_wide && c->has_add_carry && c->has_ovf_ops && c->has_bitops && c->has_switch) {
        return true;
    }

    for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
        PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;

        uint32_t orig_blocks = fn->blocks.len;
        for (uint32_t b = 0u; b < orig_blocks; ++b) {
            PirInstIdVec new_insts = {0};
            uint32_t old_len = fn->blocks.data[b].insts.len;

            for (uint32_t j = 0u; j < old_len; ++j) {
                PirInstId iid = fn->blocks.data[b].insts.data[j];
                PirInst copy  = fn->insts.data[iid];
                uint32_t ln = copy.src_line, lc = copy.loc_idx;

                if (!c->has_ovf_ops &&
                    (copy.op == PIR_OP_ADD_OVF_S || copy.op == PIR_OP_ADD_OVF_U ||
                     copy.op == PIR_OP_SUB_OVF_S || copy.op == PIR_OP_SUB_OVF_U ||
                     copy.op == PIR_OP_MUL_OVF_S || copy.op == PIR_OP_MUL_OVF_U)) {
                    expand_ovf_or_carry(mod, fn, b, &new_insts, &copy);
                } else if ((!c->has_ovf_ops || !c->has_add_carry) &&
                           (copy.op == PIR_OP_ADD_CARRY || copy.op == PIR_OP_SUB_BORROW)) {
                    expand_ovf_or_carry(mod, fn, b, &new_insts, &copy);
                } else if (!c->has_mul_wide &&
                           (copy.op == PIR_OP_MUL_WIDE_U || copy.op == PIR_OP_MUL_WIDE_S)) {
                    bool is_s = (copy.op == PIR_OP_MUL_WIDE_S);
                    if (copy.ty.u.scalar == PIR_SCALAR_I32) {
                        PirType t64 = pir_type_scalar(PIR_SCALAR_I64);
                        PirOpcode ext = is_s ? PIR_OP_SEXT : PIR_OP_ZEXT;
                        PirOperand va = ensure_val(mod, fn, b, &new_insts, copy.operands[0], ln, lc);
                        PirOperand vb = ensure_val(mod, fn, b, &new_insts, copy.operands[1], ln, lc);
                        PirValueId wa = emit1(mod, fn, b, &new_insts, ext, t64, va, ln, lc, PIR_INVALID_ID);
                        PirValueId wb = emit1(mod, fn, b, &new_insts, ext, t64, vb, ln, lc, PIR_INVALID_ID);
                        PirValueId wp = emit2(mod, fn, b, &new_insts, PIR_OP_MUL, t64, 0u, pir_op_val(wa, t64), pir_op_val(wb, t64), ln, lc, PIR_INVALID_ID);
                        emit1(mod, fn, b, &new_insts, PIR_OP_TRUNC, copy.ty, pir_op_val(wp, t64), ln, lc, copy.results[0]);
                        PirValueId sh = emit2(mod, fn, b, &new_insts, PIR_OP_SHR, t64, PIR_FLAG_MASK, pir_op_val(wp, t64), pir_op_imm_i(32u, false, t64), ln, lc, PIR_INVALID_ID);
                        emit1(mod, fn, b, &new_insts, PIR_OP_TRUNC, copy.ty, pir_op_val(sh, t64), ln, lc, copy.results[1]);
                    } else {
                        expand_mul_wide_64(mod, fn, b, &new_insts, is_s, copy.operands[0], copy.operands[1],
                                           copy.results[0], copy.results[1], ln, lc);
                    }
                } else if (!c->has_bitops &&
                           (copy.op == PIR_OP_CLZ || copy.op == PIR_OP_CTZ ||
                            copy.op == PIR_OP_POPCNT || copy.op == PIR_OP_BSWAP)) {
                    expand_bitop(mod, fn, b, &new_insts, &copy);
                } else if (!c->has_switch && copy.op == PIR_OP_SWITCH) {
                    uint32_t nc = copy.switch_case_count;
                    SortedCase *sc = nc ? (SortedCase *)malloc(nc * sizeof(SortedCase)) : NULL;
                    unsigned bits = pir_scalar_bits(copy.ty.u.scalar, target->ptr_bytes);
                    for (uint32_t k = 0u; k < nc; ++k) {
                        uint64_t raw = copy.switch_cases[k].is_negative
                                       ? (uint64_t)(-(int64_t)copy.switch_cases[k].case_lo)
                                       : copy.switch_cases[k].case_lo;
                        if (bits < 64u && (raw & ((uint64_t)1u << (bits - 1u))) != 0u) {
                            raw |= ~(((uint64_t)1u << bits) - 1u);
                        }
                        sc[k].key = (int64_t)raw;
                        sc[k].sc  = copy.switch_cases[k];
                    }
                    if (nc > 1u) qsort(sc, nc, sizeof(SortedCase), cmp_sorted_case);
                    emit_switch_node(mod, fn, b, &new_insts, copy.ty, copy.operands[0],
                                     sc, 0, (int)nc - 1, copy.targets[0], ln, lc);
                    free(sc);
                } else {
                    PIR_VEC_PUSH(&new_insts, iid);
                }
            }
            PIR_VEC_FREE(&fn->blocks.data[b].insts);
            fn->blocks.data[b].insts = new_insts;
        }
    }
    return true;
}
