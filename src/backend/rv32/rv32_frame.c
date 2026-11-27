/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RISC-V 32-bit (RV32IMAFD / ILP32D) Frame Layout & Prologue/Epilogue Lowering.
 *
 * Frame layout (fp = s0 = incoming caller sp, sp always 16-byte aligned):
 *   [fp + 0, fp + ...]            : incoming stack arguments
 *   [fp - va_save_bytes .. fp - 4]: vararg GP save area (a[va_fixed_gp]..a7)
 *   [fp - va_save - 4]            : saved ra
 *   [fp - va_save - 8]            : saved s0 (previous fp)
 *   [fp - ...]                    : saved callee-saved GP (s1..s11) & FP (fs0..fs11)
 *   [fp - ...]                    : local allocas & spill slots
 *   [sp + 0 .. sp + out_args - 1] : outgoing stack args (when !has_alloca_dyn)
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool fits_simm12(int64_t v) {
    return v >= -2048 && v <= 2047;
}

static void emit_add_off(MInstVec *out, MPReg dst, MPReg base, int64_t off,
                         MPReg scratch, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            MInst mv = minst_make(MI_RV32_MV, 4u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_preg_def(dst); mv.ops[1] = mop_preg_use(base);
            PIR_VEC_PUSH(out, mv);
        }
        return;
    }
    if (fits_simm12(off)) {
        MInst ai = minst_make(MI_RV32_ADDI, 4u, loc);
        ai.op_count = 3u; ai.ops[0] = mop_preg_def(dst); ai.ops[1] = mop_preg_use(base);
        ai.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, ai);
        return;
    }
    MInst li = minst_make(MI_RV32_LI, 4u, loc);
    li.op_count = 2u; li.ops[0] = mop_preg_def(scratch); li.ops[1] = mop_imm(off);
    PIR_VEC_PUSH(out, li);
    MInst ad = minst_make(MI_RV32_ADD, 4u, loc);
    ad.op_count = 3u; ad.ops[0] = mop_preg_def(dst); ad.ops[1] = mop_preg_use(base);
    ad.ops[2] = mop_preg_use(scratch);
    PIR_VEC_PUSH(out, ad);
}

static void emit_mem_at_off(MInstVec *out, bool is_load, MPReg reg,
                            MPReg base, int64_t off, uint8_t width,
                            MPReg addr_scratch, uint32_t loc) {
    bool is_fp = rv32_is_fp_preg(reg);
    MOpcode op = is_load ? (is_fp ? MI_RV32_FLD : MI_RV32_LD)
                         : (is_fp ? MI_RV32_FST : MI_RV32_ST);
    if (fits_simm12(off)) {
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

static void emit_sp_adj(MInstVec *out, int64_t delta, uint32_t loc) {
    if (delta == 0) return;
    emit_add_off(out, RV32_SP, RV32_SP, delta, RV32_SCRATCH_GP1, loc);
}

bool pir_rv32_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;
    if (mf->blocks.len == 0u) return true;

    /* Collect callee-saved GP (s1, s2..s11) and FP (fs0..fs11) registers used. */
    MPReg cs_gp[16];
    uint32_t n_cs_gp = 0u;
    if ((mf->used_callee_gp_mask & (1u << 9u)) != 0u) {
        cs_gp[n_cs_gp++] = RV32_S1;
    }
    for (uint32_t r = 18u; r <= 27u; ++r) {
        if ((mf->used_callee_gp_mask & (1u << r)) != 0u) {
            cs_gp[n_cs_gp++] = RV32_GP(r);
        }
    }

    MPReg cs_fp[16];
    uint32_t n_cs_fp = 0u;
    for (uint32_t r = 8u; r <= 9u; ++r) {
        if ((mf->used_callee_fp_mask & (1u << r)) != 0u) {
            cs_fp[n_cs_fp++] = RV32_FP(r);
        }
    }
    for (uint32_t r = 18u; r <= 27u; ++r) {
        if ((mf->used_callee_fp_mask & (1u << r)) != 0u) {
            cs_fp[n_cs_fp++] = RV32_FP(r);
        }
    }

    uint64_t va_save_bytes = 0u;
    if (mf->is_variadic && mf->va_fixed_gp < 8u) {
        va_save_bytes = (uint64_t)(8u - mf->va_fixed_gp) * 4u;
    }

    /* Check if leaf function can skip frame setup entirely. */
    bool need_frame = mf->has_calls || mf->has_alloca_dyn || mf->is_variadic ||
                      mf->slots.len > 0u || n_cs_gp > 0u || n_cs_fp > 0u ||
                      mf->max_out_args_bytes > 0u;

    /* Assign negative offsets below fp (= caller sp). */
    uint64_t cur_neg = va_save_bytes;
    int64_t ra_off = 0, s0_off = 0;
    int64_t cs_gp_off[16];
    int64_t cs_fp_off[16];

    if (need_frame) {
        cur_neg += 4u; ra_off = -(int64_t)cur_neg;
        cur_neg += 4u; s0_off = -(int64_t)cur_neg;
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
    uint64_t total_frame = need_frame ? align_up_u64(cur_neg + static_out, 16u) : 0u;
    mf->local_frame_size = total_frame;

    /* Rewrite every block: expand pseudo-ops, insert prologue in blk 0, epilogue before MI_RV32_RET. */
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u && need_frame) {
            /* Prologue:
             *   sp -= total_frame
             *   t5 = sp + total_frame  (= caller_sp)
             *   sw ra, ra_off(t5); sw s0, s0_off(t5)
             *   mv s0, t5              (fp = caller_sp)
             *   save vararg aK..a7 and callee-saved regs relative to fp (s0) */
            emit_sp_adj(&out, -(int64_t)total_frame, 0u);
            emit_add_off(&out, RV32_SCRATCH_GP1, RV32_SP, (int64_t)total_frame, RV32_SCRATCH_GP0, 0u);
            emit_mem_at_off(&out, false, RV32_RA, RV32_SCRATCH_GP1, ra_off, 4u, RV32_SCRATCH_GP0, 0u);
            emit_mem_at_off(&out, false, RV32_S0, RV32_SCRATCH_GP1, s0_off, 4u, RV32_SCRATCH_GP0, 0u);
            MInst mfp = minst_make(MI_RV32_MV, 4u, 0u);
            mfp.op_count = 2u; mfp.ops[0] = mop_preg_def(RV32_S0); mfp.ops[1] = mop_preg_use(RV32_SCRATCH_GP1);
            PIR_VEC_PUSH(&out, mfp);

            if (mf->is_variadic && mf->va_fixed_gp < 8u) {
                uint32_t nva = 8u - mf->va_fixed_gp;
                for (uint32_t k = 0u; k < nva; ++k) {
                    int64_t voff = -(int64_t)((nva - k) * 4u);
                    emit_mem_at_off(&out, false, RV32_GP(10u + mf->va_fixed_gp + k),
                                    RV32_S0, voff, 4u, RV32_SCRATCH_GP1, 0u);
                }
            }
            for (uint32_t i = 0u; i < n_cs_gp; ++i) {
                emit_mem_at_off(&out, false, cs_gp[i], RV32_S0, cs_gp_off[i], 4u, RV32_SCRATCH_GP1, 0u);
            }
            for (uint32_t i = 0u; i < n_cs_fp; ++i) {
                emit_mem_at_off(&out, false, cs_fp[i], RV32_S0, cs_fp_off[i], 8u, RV32_SCRATCH_GP1, 0u);
            }
        }

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            uint32_t loc = ins.loc_idx;

            if (ins.op == MI_COPY) {
                MPReg d = ins.ops[0].u.preg;
                MPReg s = ins.ops[1].u.preg;
                if (d == s) continue;
                MInst mv = minst_make(rv32_is_fp_preg(d) ? MI_RV32_FMV_RR : MI_RV32_MV, ins.width, loc);
                mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                PIR_VEC_PUSH(&out, mv);
                continue;
            }
            if (ins.op == MI_FRAME_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset + ins.ops[2].u.imm;
                emit_add_off(&out, dst, RV32_S0, off, RV32_SCRATCH_GP1, loc);
                continue;
            }
            if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                emit_mem_at_off(&out, true, dst, RV32_S0, off, ins.width, RV32_SCRATCH_GP1, loc);
                continue;
            }
            if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                MPReg atmp = (src == RV32_SCRATCH_GP1) ? RV32_SCRATCH_GP2 : RV32_SCRATCH_GP1;
                emit_mem_at_off(&out, false, src, RV32_S0, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = ins.ops[1].u.imm;
                MPReg base = need_frame ? RV32_S0 : RV32_SP;
                emit_mem_at_off(&out, true, dst, base, off, ins.width, RV32_SCRATCH_GP1, loc);
                continue;
            }
            if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = ins.ops[1].u.imm;
                MPReg atmp = (src == RV32_SCRATCH_GP1) ? RV32_SCRATCH_GP2 : RV32_SCRATCH_GP1;
                emit_mem_at_off(&out, false, src, RV32_SP, off, ins.width, atmp, loc);
                continue;
            }
            if (ins.op == MI_VA_START) {
                MPReg ap_ptr = ins.ops[0].u.preg;
                MPReg atmp = (ap_ptr == RV32_SCRATCH_GP1) ? RV32_SCRATCH_GP2 : RV32_SCRATCH_GP1;
                int64_t va_off = (mf->va_fixed_gp < 8u)
                                 ? -(int64_t)((8u - mf->va_fixed_gp) * 4u)
                                 : (int64_t)mf->va_fixed_stack_bytes;
                emit_add_off(&out, atmp, RV32_S0, va_off, atmp, loc);
                MInst st = minst_make(MI_RV32_ST, 4u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_preg_use(atmp);
                st.ops[1] = mop_preg_use(ap_ptr);
                st.ops[2] = mop_imm(0);
                PIR_VEC_PUSH(&out, st);
                continue;
            }
            if (ins.op == MI_CALL_ADJ_DOWN) {
                if (mf->has_alloca_dyn) {
                    emit_sp_adj(&out, -ins.ops[0].u.imm, loc);
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_UP) {
                if (mf->has_alloca_dyn) {
                    emit_sp_adj(&out, ins.ops[0].u.imm, loc);
                }
                continue;
            }
            if (ins.op == MI_RV32_RET) {
                if (need_frame) {
                    for (uint32_t i = n_cs_fp; i > 0u; --i) {
                        emit_mem_at_off(&out, true, cs_fp[i - 1u], RV32_S0, cs_fp_off[i - 1u],
                                        8u, RV32_SCRATCH_GP1, loc);
                    }
                    for (uint32_t i = n_cs_gp; i > 0u; --i) {
                        emit_mem_at_off(&out, true, cs_gp[i - 1u], RV32_S0, cs_gp_off[i - 1u],
                                        4u, RV32_SCRATCH_GP1, loc);
                    }
                    emit_mem_at_off(&out, true, RV32_RA, RV32_S0, ra_off, 4u, RV32_SCRATCH_GP1, loc);
                    /* Save caller_sp (fp) in t5, restore previous s0, then sp = t5. */
                    MInst sv = minst_make(MI_RV32_MV, 4u, loc);
                    sv.op_count = 2u; sv.ops[0] = mop_preg_def(RV32_SCRATCH_GP1); sv.ops[1] = mop_preg_use(RV32_S0);
                    PIR_VEC_PUSH(&out, sv);
                    emit_mem_at_off(&out, true, RV32_S0, RV32_SCRATCH_GP1, s0_off, 4u, RV32_SCRATCH_GP0, loc);
                    MInst rsp = minst_make(MI_RV32_MV, 4u, loc);
                    rsp.op_count = 2u; rsp.ops[0] = mop_preg_def(RV32_SP); rsp.ops[1] = mop_preg_use(RV32_SCRATCH_GP1);
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
