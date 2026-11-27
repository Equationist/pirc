/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * PowerPC 32-bit Big-Endian (SysV SVR4) Frame Layout & Prologue/Epilogue Lowering.
 *
 * SysV PPC32 frame layout (fp = r31 = incoming caller sp, sp = r1 always 16B-aligned):
 *   [fp + 8, fp + ...]            : incoming stack arguments
 *   [fp + 4]                      : saved LR (stored in caller's LR save word)
 *   [fp + 0]                      : caller's backchain word
 *   [fp - 96 .. fp - 1]           : vararg reg_save_area (r3..r10 @+0..+28, f1..f8 @+32..+92)
 *   [fp - ...]                    : saved r31 (previous fp)
 *   [fp - ...]                    : saved callee-saved GP (r14..r30) & FP (f14..f31)
 *   [fp - ...]                    : local allocas & spill slots
 *   [sp + 8 .. sp + 7 + out_args] : outgoing stack args (when !has_alloca_dyn)
 *   [sp + 4]                      : callee's LR save word
 *   [sp + 0]                      : backchain pointer (= fp)
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool fits_simm16(int64_t v) {
    return v >= -32768 && v <= 32767;
}

static void emit_add_off(MInstVec *out, MPReg dst, MPReg base, int64_t off,
                         MPReg scratch, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            MInst mr = minst_make(MI_PPC_MR, 4u, loc);
            mr.op_count = 2u; mr.ops[0] = mop_preg_def(dst); mr.ops[1] = mop_preg_use(base);
            PIR_VEC_PUSH(out, mr);
        }
        return;
    }
    if (fits_simm16(off)) {
        MInst ai = minst_make(MI_PPC_ADDI, 4u, loc);
        ai.op_count = 3u; ai.ops[0] = mop_preg_def(dst); ai.ops[1] = mop_preg_use(base);
        ai.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, ai);
        return;
    }
    MInst li = minst_make(MI_PPC_LI, 4u, loc);
    li.op_count = 2u; li.ops[0] = mop_preg_def(scratch); li.ops[1] = mop_imm(off);
    PIR_VEC_PUSH(out, li);
    MInst ad = minst_make(MI_PPC_ADD, 4u, loc);
    ad.op_count = 3u; ad.ops[0] = mop_preg_def(dst); ad.ops[1] = mop_preg_use(base);
    ad.ops[2] = mop_preg_use(scratch);
    PIR_VEC_PUSH(out, ad);
}

static void emit_mem_at_off(MInstVec *out, bool is_load, MPReg reg,
                            MPReg base, int64_t off, uint8_t width,
                            MPReg addr_scratch, uint32_t loc) {
    bool is_fp = ppc_is_fp_preg(reg);
    MOpcode op = is_load ? (is_fp ? MI_PPC_FLD : MI_PPC_LD)
                         : (is_fp ? MI_PPC_FST : MI_PPC_ST);
    if (fits_simm16(off)) {
        MInst m = minst_make(op, width, loc);
        m.op_count = 3u;
        m.ops[0] = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
        m.ops[1] = mop_preg_use(base);
        m.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, m);
        return;
    }
    emit_add_off(out, addr_scratch, base, off, addr_scratch, loc);
    MInst m = minst_make(op, width, loc);
    m.op_count = 3u;
    m.ops[0] = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
    m.ops[1] = mop_preg_use(addr_scratch);
    m.ops[2] = mop_imm(0);
    PIR_VEC_PUSH(out, m);
}

bool pir_ppc32_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;
    if (mf->blocks.len == 0u) return true;

    /* Collect callee-saved GP (r14..r30) and FP (f14..f31) registers used. */
    MPReg cs_gp[20];
    uint32_t n_cs_gp = 0u;
    for (uint32_t r = 14u; r <= 30u; ++r) {
        if ((mf->used_callee_gp_mask & (1u << r)) != 0u) {
            cs_gp[n_cs_gp++] = PPC_GP(r);
        }
    }

    MPReg cs_fp[20];
    uint32_t n_cs_fp = 0u;
    for (uint32_t r = 14u; r <= 31u; ++r) {
        if ((mf->used_callee_fp_mask & (1u << r)) != 0u) {
            cs_fp[n_cs_fp++] = PPC_FP(r);
        }
    }

    bool need_frame = mf->has_calls || mf->has_alloca_dyn || mf->is_variadic ||
                      mf->slots.len > 0u || n_cs_gp > 0u || n_cs_fp > 0u ||
                      mf->max_out_args_bytes > 0u;

    uint64_t cur_neg = 0u;
    int64_t  fp_save_off = 0;
    int64_t  cs_gp_off[20];
    int64_t  cs_fp_off[20];

    if (need_frame) {
        if (mf->is_variadic) {
            /* 96-byte 8B-aligned reg_save_area: r3..r10 (32B) + f1..f8 (64B). */
            cur_neg = align_up_u64(cur_neg, 8u) + 96u;
            mf->ppc_reg_save_off = -(int64_t)cur_neg;
        }
        cur_neg += 4u;
        fp_save_off = -(int64_t)cur_neg;

        for (uint32_t i = 0u; i < n_cs_gp; ++i) {
            cur_neg += 4u;
            cs_gp_off[i] = -(int64_t)cur_neg;
        }
        if (n_cs_fp > 0u) {
            cur_neg = align_up_u64(cur_neg, 8u);
            for (uint32_t i = 0u; i < n_cs_fp; ++i) {
                cur_neg += 8u;
                cs_fp_off[i] = -(int64_t)cur_neg;
            }
        }
        for (uint32_t s = 0u; s < mf->slots.len; ++s) {
            uint64_t al = mf->slots.data[s].align > 0u ? mf->slots.data[s].align : 1u;
            if (al > 16u) al = 16u;
            cur_neg = align_up_u64(cur_neg + mf->slots.data[s].size, al);
            mf->slots.data[s].fp_offset = -(int64_t)cur_neg;
        }
    }

    uint64_t static_out = mf->has_alloca_dyn ? 0u : mf->max_out_args_bytes;
    /* Reserve at least 8 bytes at sp+0 (backchain) and sp+4 (callee LR save). */
    uint64_t total_frame = need_frame ? align_up_u64(cur_neg + 8u + static_out, 16u) : 0u;
    mf->local_frame_size = total_frame;

    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u && need_frame) {
            /* Prologue:
             *   mflr 0; stw 0, 4(1)           (save LR in caller's LR slot)
             *   mr 11, 1                      (r11 = caller_sp)
             *   stwu 1, -total_frame(1)       (or large frame sub + stw 11, 0(1))
             *   stw 31, fp_save_off(11)       (save old fp)
             *   mr 31, 11                     (fp = caller_sp)
             *   [vararg reg_save_area]
             *   [callee-saved GP & FP] */
            MInst mflr = minst_make(MI_PPC_MFLR, 4u, 0u);
            mflr.op_count = 1u; mflr.ops[0] = mop_preg_def(PPC_R0);
            PIR_VEC_PUSH(&out, mflr);

            MInst stlr = minst_make(MI_PPC_ST, 4u, 0u);
            stlr.op_count = 3u; stlr.ops[0] = mop_preg_use(PPC_R0);
            stlr.ops[1] = mop_preg_use(PPC_SP); stlr.ops[2] = mop_imm(4);
            PIR_VEC_PUSH(&out, stlr);

            MInst svsp = minst_make(MI_PPC_MR, 4u, 0u);
            svsp.op_count = 2u; svsp.ops[0] = mop_preg_def(PPC_SCRATCH_GP0);
            svsp.ops[1] = mop_preg_use(PPC_SP);
            PIR_VEC_PUSH(&out, svsp);

            if (total_frame <= 32768u) {
                MInst stwu = minst_make(MI_PPC_STWU, 4u, 0u);
                stwu.op_count = 1u; stwu.ops[0] = mop_imm(-(int64_t)total_frame);
                PIR_VEC_PUSH(&out, stwu);
            } else {
                emit_add_off(&out, PPC_SP, PPC_SP, -(int64_t)total_frame, PPC_SCRATCH_GP1, 0u);
                MInst sbc = minst_make(MI_PPC_ST, 4u, 0u);
                sbc.op_count = 3u; sbc.ops[0] = mop_preg_use(PPC_SCRATCH_GP0);
                sbc.ops[1] = mop_preg_use(PPC_SP); sbc.ops[2] = mop_imm(0);
                PIR_VEC_PUSH(&out, sbc);
            }

            emit_mem_at_off(&out, false, PPC_FP_REG, PPC_SCRATCH_GP0, fp_save_off, 4u, PPC_SCRATCH_GP1, 0u);

            MInst mfp = minst_make(MI_PPC_MR, 4u, 0u);
            mfp.op_count = 2u; mfp.ops[0] = mop_preg_def(PPC_FP_REG);
            mfp.ops[1] = mop_preg_use(PPC_SCRATCH_GP0);
            PIR_VEC_PUSH(&out, mfp);

            if (mf->is_variadic) {
                for (uint32_t k = 0u; k < 8u; ++k) {
                    emit_mem_at_off(&out, false, PPC_GP(3u + k), PPC_FP_REG,
                                    mf->ppc_reg_save_off + (int64_t)(k * 4u), 4u, PPC_SCRATCH_GP1, 0u);
                }
                for (uint32_t k = 0u; k < 8u; ++k) {
                    emit_mem_at_off(&out, false, PPC_FP(1u + k), PPC_FP_REG,
                                    mf->ppc_reg_save_off + 32 + (int64_t)(k * 8u), 8u, PPC_SCRATCH_GP1, 0u);
                }
            }
            for (uint32_t i = 0u; i < n_cs_gp; ++i) {
                emit_mem_at_off(&out, false, cs_gp[i], PPC_FP_REG, cs_gp_off[i], 4u, PPC_SCRATCH_GP1, 0u);
            }
            for (uint32_t i = 0u; i < n_cs_fp; ++i) {
                emit_mem_at_off(&out, false, cs_fp[i], PPC_FP_REG, cs_fp_off[i], 8u, PPC_SCRATCH_GP1, 0u);
            }
        }

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            uint32_t loc = ins.loc_idx;

            if (ins.op == MI_COPY) {
                MPReg d = ins.ops[0].u.preg;
                MPReg s = ins.ops[1].u.preg;
                if (d == s) continue;
                MInst mv = minst_make(ppc_is_fp_preg(d) ? MI_PPC_FMR : MI_PPC_MR, ins.width, loc);
                mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                PIR_VEC_PUSH(&out, mv);
                continue;
            }
            if (ins.op == MI_FRAME_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset + ins.ops[2].u.imm;
                emit_add_off(&out, dst, PPC_FP_REG, off, dst, loc);
                continue;
            }
            if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                MPReg atmp = ppc_is_fp_preg(dst) ? PPC_SCRATCH_GP1 : dst;
                emit_mem_at_off(&out, true, dst, PPC_FP_REG, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                MPReg atmp = (src == PPC_SCRATCH_GP1) ? PPC_SCRATCH_GP0 : PPC_SCRATCH_GP1;
                emit_mem_at_off(&out, false, src, PPC_FP_REG, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = 8 + ins.ops[1].u.imm;
                MPReg base = need_frame ? PPC_FP_REG : PPC_SP;
                MPReg atmp = ppc_is_fp_preg(dst) ? PPC_SCRATCH_GP1 : dst;
                emit_mem_at_off(&out, true, dst, base, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = 8 + ins.ops[1].u.imm;
                MPReg atmp = (src == PPC_SCRATCH_GP1) ? PPC_SCRATCH_GP0 : PPC_SCRATCH_GP1;
                emit_mem_at_off(&out, false, src, PPC_SP, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_VA_START) {
                /* SysV PPC32 va_list (12B):
                 *   @0: uint8_t gpr  (min(va_fixed_gp, 8))
                 *   @1: uint8_t fpr  (min(va_fixed_fp, 8))
                 *   @2: uint16_t pad (0)
                 *   @4: ptr overflow_arg_area (fp + 8 + va_fixed_stack_bytes)
                 *   @8: ptr reg_save_area     (fp + ppc_reg_save_off) */
                MPReg ap_ptr = ins.ops[0].u.preg;
                MPReg tmp = (ap_ptr == PPC_SCRATCH_GP1) ? PPC_SCRATCH_GP0 : PPC_SCRATCH_GP1;

                uint32_t gpr_n = mf->va_fixed_gp < 8u ? mf->va_fixed_gp : 8u;
                uint32_t fpr_n = mf->va_fixed_fp < 8u ? mf->va_fixed_fp : 8u;
                /* Pack big-endian first word: (gpr << 24) | (fpr << 16) */
                uint32_t hdr_word = (gpr_n << 24u) | (fpr_n << 16u);
                MInst l0 = minst_make(MI_PPC_LI, 4u, loc);
                l0.op_count = 2u; l0.ops[0] = mop_preg_def(tmp); l0.ops[1] = mop_imm((int64_t)(int32_t)hdr_word);
                PIR_VEC_PUSH(&out, l0);
                MInst s0 = minst_make(MI_PPC_ST, 4u, loc);
                s0.op_count = 3u; s0.ops[0] = mop_preg_use(tmp); s0.ops[1] = mop_preg_use(ap_ptr); s0.ops[2] = mop_imm(0);
                PIR_VEC_PUSH(&out, s0);

                int64_t oaa_off = 8 + (int64_t)mf->va_fixed_stack_bytes;
                emit_add_off(&out, tmp, PPC_FP_REG, oaa_off, tmp, loc);
                MInst s1 = minst_make(MI_PPC_ST, 4u, loc);
                s1.op_count = 3u; s1.ops[0] = mop_preg_use(tmp); s1.ops[1] = mop_preg_use(ap_ptr); s1.ops[2] = mop_imm(4);
                PIR_VEC_PUSH(&out, s1);

                emit_add_off(&out, tmp, PPC_FP_REG, mf->ppc_reg_save_off, tmp, loc);
                MInst s2 = minst_make(MI_PPC_ST, 4u, loc);
                s2.op_count = 3u; s2.ops[0] = mop_preg_use(tmp); s2.ops[1] = mop_preg_use(ap_ptr); s2.ops[2] = mop_imm(8);
                PIR_VEC_PUSH(&out, s2);
                continue;
            }
            if (ins.op == MI_CALL_ADJ_DOWN) {
                if (mf->has_alloca_dyn) {
                    /* Add 16B header margin so sp+8+out_bytes never touches old 0(sp) backchain. */
                    int64_t adj = ins.ops[0].u.imm + 16;
                    MInst lbc = minst_make(MI_PPC_LD, 4u, loc);
                    lbc.op_count = 3u; lbc.ops[0] = mop_preg_def(PPC_SCRATCH_GP0);
                    lbc.ops[1] = mop_preg_use(PPC_SP); lbc.ops[2] = mop_imm(0);
                    PIR_VEC_PUSH(&out, lbc);
                    emit_add_off(&out, PPC_SP, PPC_SP, -adj, PPC_SCRATCH_GP1, loc);
                    MInst sbc = minst_make(MI_PPC_ST, 4u, loc);
                    sbc.op_count = 3u; sbc.ops[0] = mop_preg_use(PPC_SCRATCH_GP0);
                    sbc.ops[1] = mop_preg_use(PPC_SP); sbc.ops[2] = mop_imm(0);
                    PIR_VEC_PUSH(&out, sbc);
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_UP) {
                if (mf->has_alloca_dyn) {
                    int64_t adj = ins.ops[0].u.imm + 16;
                    emit_add_off(&out, PPC_SP, PPC_SP, adj, PPC_SCRATCH_GP1, loc);
                }
                continue;
            }
            if (ins.op == MI_PPC_RET) {
                if (need_frame) {
                    for (uint32_t i = n_cs_fp; i > 0u; --i) {
                        emit_mem_at_off(&out, true, cs_fp[i - 1u], PPC_FP_REG, cs_fp_off[i - 1u],
                                        8u, PPC_SCRATCH_GP1, loc);
                    }
                    for (uint32_t i = n_cs_gp; i > 0u; --i) {
                        emit_mem_at_off(&out, true, cs_gp[i - 1u], PPC_FP_REG, cs_gp_off[i - 1u],
                                        4u, PPC_SCRATCH_GP1, loc);
                    }
                    MInst ldlr = minst_make(MI_PPC_LD, 4u, loc);
                    ldlr.op_count = 3u; ldlr.ops[0] = mop_preg_def(PPC_R0);
                    ldlr.ops[1] = mop_preg_use(PPC_FP_REG); ldlr.ops[2] = mop_imm(4);
                    PIR_VEC_PUSH(&out, ldlr);

                    MInst mtlr = minst_make(MI_PPC_MTLR, 4u, loc);
                    mtlr.op_count = 1u; mtlr.ops[0] = mop_preg_use(PPC_R0);
                    PIR_VEC_PUSH(&out, mtlr);

                    MInst sv = minst_make(MI_PPC_MR, 4u, loc);
                    sv.op_count = 2u; sv.ops[0] = mop_preg_def(PPC_SCRATCH_GP0);
                    sv.ops[1] = mop_preg_use(PPC_FP_REG);
                    PIR_VEC_PUSH(&out, sv);

                    emit_mem_at_off(&out, true, PPC_FP_REG, PPC_SCRATCH_GP0, fp_save_off,
                                    4u, PPC_SCRATCH_GP1, loc);

                    MInst rsp = minst_make(MI_PPC_MR, 4u, loc);
                    rsp.op_count = 2u; rsp.ops[0] = mop_preg_def(PPC_SP);
                    rsp.ops[1] = mop_preg_use(PPC_SCRATCH_GP0);
                    PIR_VEC_PUSH(&out, rsp);
                }
                PIR_VEC_PUSH(&out, ins);
                continue;
            }

            PIR_VEC_PUSH(&out, ins);
        }

        PIR_VEC_FREE(&blk->insts);
        blk->insts = out;
    }
    return true;
}
