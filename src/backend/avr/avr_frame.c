/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * AVR (ATmega1280) Frame Layout & Prologue/Epilogue Lowering.
 *
 * ATmega1280 stack layout (2-byte PC, SP points 1 byte BELOW top of stack, Y = r28:r29 = FP):
 *   [Y + 1 .. Y + static_out]                      : outgoing stack args (when !has_alloca_dyn)
 *   [Y + 1 + static_out .. Y + local_sz]           : local allocas & spill slots
 *   [Y + local_sz + 1 .. Y + local_sz + saved_sz]  : saved callee-saved pairs + saved Y (r28:r29)
 *   [Y + local_sz + saved_sz + 1 .. +2]            : 2-byte Flash return address
 *   [Y + local_sz + saved_sz + 3 ..]               : incoming stack arguments
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

static void emit_add_imm(MInstVec *out, MPReg dst, MPReg base, int64_t off, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            MInst mv = minst_make(MI_AVR_MOV, 2u, loc);
            mv.op_count = 2u; mv.ops[0] = mop_preg_def(dst); mv.ops[1] = mop_preg_use(base);
            PIR_VEC_PUSH(out, mv);
        }
        return;
    }
    MInst ai = minst_make(MI_AVR_ADDI, 2u, loc);
    ai.op_count = 3u;
    ai.ops[0] = mop_preg_def(dst);
    ai.ops[1] = mop_preg_use(base);
    ai.ops[2] = mop_imm((int64_t)(int16_t)off);
    PIR_VEC_PUSH(out, ai);
}

bool pir_avr_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;
    if (mf->blocks.len == 0u) return true;

    /* Collect used callee-saved GP pairs (r2:r3 .. r16:r17 -> pairs 1..8). */
    MPReg cs_gp[8];
    uint32_t n_cs_gp = 0u;
    for (uint32_t p = 1u; p <= 8u; ++p) {
        if ((mf->used_callee_gp_mask & (1u << p)) != 0u) {
            cs_gp[n_cs_gp++] = AVR_PAIR(p);
        }
    }

    bool has_inarg = false;
    for (uint32_t b = 0u; b < mf->blocks.len && !has_inarg; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            if (blk->insts.data[j].op == MI_LOAD_INARG) {
                has_inarg = true;
                break;
            }
        }
    }

    bool need_frame = mf->has_alloca_dyn || mf->is_variadic || has_inarg ||
                      mf->slots.len > 0u || n_cs_gp > 0u ||
                      mf->max_out_args_bytes > 0u;

    uint64_t static_out = mf->has_alloca_dyn ? 0u : mf->max_out_args_bytes;
    uint64_t cur_off = static_out;
    for (uint32_t s = 0u; s < mf->slots.len; ++s) {
        /* Positive offset from Y: Y+1 is the first byte of the frame. */
        mf->slots.data[s].fp_offset = 1 + (int64_t)cur_off;
        cur_off += mf->slots.data[s].size;
    }

    uint64_t local_sz = need_frame ? cur_off : 0u;
    uint64_t saved_sz = need_frame ? (2u + (uint64_t)n_cs_gp * 2u) : 0u;
    int64_t  inarg_base = (int64_t)(local_sz + saved_sz + 3u);
    mf->local_frame_size = local_sz;

    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u && need_frame) {
            /* Prologue: push Y, push callee-saved pairs, SP -= local_sz, Y = SP. */
            MInst py = minst_make(MI_AVR_PUSH, 2u, 0u);
            py.op_count = 1u; py.ops[0] = mop_preg_use(AVR_FP_REG);
            PIR_VEC_PUSH(&out, py);

            for (uint32_t i = 0u; i < n_cs_gp; ++i) {
                MInst pc = minst_make(MI_AVR_PUSH, 2u, 0u);
                pc.op_count = 1u; pc.ops[0] = mop_preg_use(cs_gp[i]);
                PIR_VEC_PUSH(&out, pc);
            }

            if (local_sz > 0u) {
                MInst adj = minst_make(MI_AVR_ADJ_SP, 2u, 0u);
                adj.op_count = 1u; adj.ops[0] = mop_imm(-(int64_t)local_sz);
                PIR_VEC_PUSH(&out, adj);
            }

            MInst iy = minst_make(MI_AVR_IN_SP, 2u, 0u);
            iy.op_count = 1u; iy.ops[0] = mop_preg_def(AVR_FP_REG);
            PIR_VEC_PUSH(&out, iy);
        }

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            uint32_t loc = ins.loc_idx;

            if (ins.op == MI_COPY) {
                MPReg d = ins.ops[0].u.preg;
                MPReg s = ins.ops[1].u.preg;
                if (d == s) continue;
                MInst mv = minst_make(MI_AVR_MOV, 2u, loc);
                mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                PIR_VEC_PUSH(&out, mv);
                continue;
            }
            if (ins.op == MI_FRAME_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset + ins.ops[2].u.imm;
                emit_add_imm(&out, dst, AVR_FP_REG, off, loc);
                continue;
            }
            if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                MInst ld = minst_make(MI_AVR_LD, ins.width, loc);
                ld.op_count = 3u;
                ld.ops[0] = mop_preg_def(dst);
                ld.ops[1] = mop_preg_use(AVR_FP_REG);
                ld.ops[2] = mop_imm(off);
                PIR_VEC_PUSH(&out, ld);
                continue;
            }
            if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                MInst st = minst_make(MI_AVR_ST, ins.width, loc);
                st.op_count = 3u;
                st.ops[0] = mop_preg_use(src);
                st.ops[1] = mop_preg_use(AVR_FP_REG);
                st.ops[2] = mop_imm(off);
                PIR_VEC_PUSH(&out, st);
                continue;
            }
            if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = inarg_base + ins.ops[1].u.imm;
                MInst ld = minst_make(MI_AVR_LD, ins.width, loc);
                ld.op_count = 3u;
                ld.ops[0] = mop_preg_def(dst);
                ld.ops[1] = mop_preg_use(AVR_FP_REG);
                ld.ops[2] = mop_imm(off);
                PIR_VEC_PUSH(&out, ld);
                continue;
            }
            if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = 1 + ins.ops[1].u.imm;
                if (mf->has_alloca_dyn) {
                    MPReg sp_tmp = (src == AVR_SCRATCH_GP1) ? AVR_SCRATCH_GP0 : AVR_SCRATCH_GP1;
                    MInst isp = minst_make(MI_AVR_IN_SP, 2u, loc);
                    isp.op_count = 1u; isp.ops[0] = mop_preg_def(sp_tmp);
                    PIR_VEC_PUSH(&out, isp);
                    MInst st = minst_make(MI_AVR_ST, ins.width, loc);
                    st.op_count = 3u;
                    st.ops[0] = mop_preg_use(src);
                    st.ops[1] = mop_preg_use(sp_tmp);
                    st.ops[2] = mop_imm(off);
                    PIR_VEC_PUSH(&out, st);
                } else {
                    MInst st = minst_make(MI_AVR_ST, ins.width, loc);
                    st.op_count = 3u;
                    st.ops[0] = mop_preg_use(src);
                    st.ops[1] = mop_preg_use(AVR_FP_REG);
                    st.ops[2] = mop_imm(off);
                    PIR_VEC_PUSH(&out, st);
                }
                continue;
            }
            if (ins.op == MI_VA_START) {
                /* AVR va_list is a 2-byte char * pointing to first variadic stack arg. */
                MPReg ap_ptr = ins.ops[0].u.preg;
                MPReg tmp = (ap_ptr == AVR_SCRATCH_GP1) ? AVR_SCRATCH_GP0 : AVR_SCRATCH_GP1;
                int64_t va_off = inarg_base + (int64_t)mf->va_fixed_stack_bytes;
                emit_add_imm(&out, tmp, AVR_FP_REG, va_off, loc);
                MInst st = minst_make(MI_AVR_ST, 2u, loc);
                st.op_count = 3u;
                st.ops[0] = mop_preg_use(tmp);
                st.ops[1] = mop_preg_use(ap_ptr);
                st.ops[2] = mop_imm(0);
                PIR_VEC_PUSH(&out, st);
                continue;
            }
            if (ins.op == MI_CALL_ADJ_DOWN) {
                if (mf->has_alloca_dyn) {
                    MInst adj = minst_make(MI_AVR_ADJ_SP, 2u, loc);
                    adj.op_count = 1u; adj.ops[0] = mop_imm(-ins.ops[0].u.imm);
                    PIR_VEC_PUSH(&out, adj);
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_UP) {
                if (mf->has_alloca_dyn) {
                    MInst adj = minst_make(MI_AVR_ADJ_SP, 2u, loc);
                    adj.op_count = 1u; adj.ops[0] = mop_imm(ins.ops[0].u.imm);
                    PIR_VEC_PUSH(&out, adj);
                }
                continue;
            }
            if (ins.op == MI_AVR_RET) {
                if (need_frame) {
                    if (mf->has_alloca_dyn) {
                        MInst osp = minst_make(MI_AVR_OUT_SP, 2u, loc);
                        osp.op_count = 1u; osp.ops[0] = mop_preg_use(AVR_FP_REG);
                        PIR_VEC_PUSH(&out, osp);
                    }
                    if (local_sz > 0u) {
                        MInst adj = minst_make(MI_AVR_ADJ_SP, 2u, loc);
                        adj.op_count = 1u; adj.ops[0] = mop_imm((int64_t)local_sz);
                        PIR_VEC_PUSH(&out, adj);
                    }
                    for (uint32_t i = n_cs_gp; i > 0u; --i) {
                        MInst pc = minst_make(MI_AVR_POP, 2u, loc);
                        pc.op_count = 1u; pc.ops[0] = mop_preg_def(cs_gp[i - 1u]);
                        PIR_VEC_PUSH(&out, pc);
                    }
                    MInst py = minst_make(MI_AVR_POP, 2u, loc);
                    py.op_count = 1u; py.ops[0] = mop_preg_def(AVR_FP_REG);
                    PIR_VEC_PUSH(&out, py);
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
