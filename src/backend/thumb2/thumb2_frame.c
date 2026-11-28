/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * ARM Thumb-2 (ARMv7E-M, Cortex-M7 FPv5-D16 / Cortex-M3/M4) Frame Lowering.
 *
 * Stack Frame Layout (8-byte SP alignment maintained at every call site):
 *
 *   High addresses:
 *     [caller outgoing stack args]       <-- r7 + gp_saved_sz + fp_saved_sz + va_save_sz
 *     [va_save: r0..r3 (16B if variadic)]
 *     [saved VFP d8..d15 (8*n_fp bytes)]
 *     [saved GP r4..r11, r7, lr (8B-aligned via pad reg if odd count)]
 *   r7 (FP) ---------------------------- <-- sp right after prologue pushes
 *     [local allocas & spill slots]      <-- negative offsets from r7 (r7 - off)
 *     [dynamic alloca_dyn region]
 *     [outgoing stack args at sp + off]
 *   Low addresses (sp)
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    if (al <= 1u) return v;
    return (v + al - 1u) & ~(al - 1u);
}

static uint32_t popcount_u32(uint32_t x) {
    uint32_t c = 0u;
    while (x != 0u) {
        c += (x & 1u);
        x >>= 1u;
    }
    return c;
}

static void emit_add_fp_off(MInstVec *out, MPReg dst, MPReg base, int64_t off, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            MInst m = minst_make(MI_T2_MOV, 4u, loc);
            m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_preg_use(base);
            PIR_VEC_PUSH(out, m);
        }
        return;
    }
    MInst ad = minst_make(MI_T2_ADDI, 4u, loc);
    ad.op_count = 3u;
    ad.ops[0] = mop_preg_def(dst);
    ad.ops[1] = mop_preg_use(base);
    ad.ops[2] = mop_imm(off);
    PIR_VEC_PUSH(out, ad);
}

static void emit_slot_load(MInstVec *out, MPReg dst, MPReg base, int64_t off, uint8_t w, uint32_t loc) {
    if (t2_is_fp_preg(dst)) {
        MInst ld = minst_make(MI_T2_VLD, (w == 8u) ? 8u : 4u, loc);
        ld.op_count = 3u;
        ld.ops[0] = mop_preg_def(dst); ld.ops[1] = mop_preg_use(base); ld.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, ld);
    } else {
        MInst ld = minst_make(MI_T2_LD, (w <= 4u && w > 0u) ? w : 4u, loc);
        ld.is_signed = false;
        ld.op_count  = 3u;
        ld.ops[0] = mop_preg_def(dst); ld.ops[1] = mop_preg_use(base); ld.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, ld);
    }
}

static void emit_slot_store(MInstVec *out, MPReg src, MPReg base, int64_t off, uint8_t w, uint32_t loc) {
    if (t2_is_fp_preg(src)) {
        MInst st = minst_make(MI_T2_VST, (w == 8u) ? 8u : 4u, loc);
        st.op_count = 3u;
        st.ops[0] = mop_preg_use(src); st.ops[1] = mop_preg_use(base); st.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, st);
    } else {
        MInst st = minst_make(MI_T2_ST, (w <= 4u && w > 0u) ? w : 4u, loc);
        st.op_count = 3u;
        st.ops[0] = mop_preg_use(src); st.ops[1] = mop_preg_use(base); st.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, st);
    }
}

bool pir_thumb2_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;

    /* 1. Assign negative r7 (FP) offsets to all local allocas & spill slots. */
    uint64_t cur = 0u;
    for (uint32_t i = 0u; i < mf->slots.len; ++i) {
        MStackSlot *sl = &mf->slots.data[i];
        uint64_t al = sl->align > 0u ? (uint64_t)sl->align : 4u;
        if (al > 8u) al = 8u; /* Over-aligned (>8B) allocas are already dynamically aligned in lower.c */
        uint64_t sz = sl->size > 0u ? sl->size : 1u;
        cur = align_up_u64(cur + sz, al);
        sl->fp_offset = -(int64_t)cur;
    }
    uint64_t local_sz = align_up_u64(cur, 8u);
    mf->local_frame_size = local_sz;

    /* 2. Scan instructions to record any implicit assembler scratch usage (r11, d15). */
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            if (ins->op == MI_T2_FCVT_I2F || ins->op == MI_T2_FCVT_F2I ||
                ins->op == MI_T2_PACK_S2) {
                if (mf->target->caps.has_hw_fp) {
                    mf->used_callee_fp_mask |= (1u << 15u); /* s30/d15 asm scratch */
                }
            }
            if (ins->op == MI_T2_ATOMIC_RMW || ins->op == MI_T2_ATOMIC_CAS ||
                (ins->op == MI_T2_BITOP && ins->aux_id == 4u) ||
                ins->op == MI_T2_LD || ins->op == MI_T2_ST ||
                ins->op == MI_T2_VLD || ins->op == MI_T2_VST ||
                ins->op == MI_LOAD_SLOT || ins->op == MI_STORE_SLOT ||
                ins->op == MI_LOAD_INARG || ins->op == MI_STORE_OUTARG ||
                ins->op == MI_T2_ADDI || ins->op == MI_T2_ANDI ||
                ins->op == MI_T2_ORI || ins->op == MI_T2_XORI) {
                mf->used_callee_gp_mask |= (1u << 11u); /* r11 secondary asm scratch */
            }
        }
    }

    /* 3. Compute saved VFP range (d8..d15) and saved GP mask (always includes r7 & lr). */
    uint32_t fp_mask = mf->target->caps.has_hw_fp ? (mf->used_callee_fp_mask & 0xFF00u) : 0u;
    uint32_t fp_first = 0u, fp_cnt = 0u;
    if (fp_mask != 0u) {
        uint32_t lo = 8u, hi = 15u;
        while (lo <= 15u && ((fp_mask & (1u << lo)) == 0u)) lo++;
        while (hi >= lo  && ((fp_mask & (1u << hi)) == 0u)) hi--;
        fp_first = lo;
        fp_cnt   = (hi - lo) + 1u;
    }
    uint64_t fp_saved_bytes = (uint64_t)fp_cnt * 8u;

    uint32_t gp_mask = (mf->used_callee_gp_mask & 0x0F70u) | (1u << 7u) | (1u << 14u); /* r4..r6,r8..r11 + r7 + lr */
    if ((popcount_u32(gp_mask) & 1u) != 0u) {
        /* Keep push.w 8-byte aligned by including one extra callee-saved GP reg! */
        static const uint32_t PAD_CANDS[] = { 4u, 5u, 6u, 8u, 9u, 10u, 11u };
        for (size_t k = 0u; k < sizeof(PAD_CANDS) / sizeof(PAD_CANDS[0]); ++k) {
            if ((gp_mask & (1u << PAD_CANDS[k])) == 0u) {
                gp_mask |= (1u << PAD_CANDS[k]);
                break;
            }
        }
    }
    uint64_t gp_saved_bytes = (uint64_t)popcount_u32(gp_mask) * 4u;
    uint64_t va_save_bytes  = mf->is_variadic ? 16u : 0u;

    /* Offset from r7 (FP) to caller's first outgoing stack argument. */
    int64_t inarg_base = (int64_t)(gp_saved_bytes + fp_saved_bytes + va_save_bytes);

    /* 4. Rewrite every block: emit prologue in block 0, lower pseudos & epilogues. */
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u) {
            if (mf->is_variadic) {
                /* Save r0..r3 right below caller stack args for contiguous va_list. */
                MInst pva = minst_make(MI_T2_PUSH, 4u, 0u);
                pva.op_count = 1u; pva.ops[0] = mop_imm(0x000Fu); /* {r0, r1, r2, r3} */
                PIR_VEC_PUSH(&out, pva);
            }
            if (fp_cnt > 0u) {
                MInst vp = minst_make(MI_T2_VPUSH, 8u, 0u);
                vp.op_count = 2u; vp.ops[0] = mop_imm((int64_t)fp_first); vp.ops[1] = mop_imm((int64_t)fp_cnt);
                PIR_VEC_PUSH(&out, vp);
            }
            {
                MInst pg = minst_make(MI_T2_PUSH, 4u, 0u);
                pg.op_count = 1u; pg.ops[0] = mop_imm((int64_t)gp_mask);
                PIR_VEC_PUSH(&out, pg);
            }
            /* Set frame pointer r7 = sp */
            {
                MInst fp = minst_make(MI_T2_MOV, 4u, 0u);
                fp.op_count = 2u; fp.ops[0] = mop_preg_def(T2_FP_REG); fp.ops[1] = mop_preg_use(T2_SP);
                PIR_VEC_PUSH(&out, fp);
            }
            if (local_sz > 0u) {
                MInst sp = minst_make(MI_T2_ADJ_SP, 4u, 0u);
                sp.op_count = 1u; sp.ops[0] = mop_imm(-(int64_t)local_sz);
                PIR_VEC_PUSH(&out, sp);
            }
        }

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            uint32_t loc = ins.loc_idx;

            if (ins.op == MI_COPY) {
                MPReg d = ins.ops[0].u.preg;
                MPReg s = ins.ops[1].u.preg;
                if (d != s) {
                    if (t2_is_fp_preg(d)) {
                        MInst mv = minst_make(MI_T2_VMOV_F, ins.width == 8u ? 8u : 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                        PIR_VEC_PUSH(&out, mv);
                    } else {
                        MInst mv = minst_make(MI_T2_MOV, 4u, loc);
                        mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                        PIR_VEC_PUSH(&out, mv);
                    }
                }
            } else if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                emit_slot_load(&out, dst, T2_FP_REG, off, ins.width, loc);
            } else if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset;
                emit_slot_store(&out, src, T2_FP_REG, off, ins.width, loc);
            } else if (ins.op == MI_FRAME_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = mf->slots.data[ins.ops[1].u.slot_id].fp_offset + ins.ops[2].u.imm;
                emit_add_fp_off(&out, dst, T2_FP_REG, off, loc);
            } else if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = inarg_base + ins.ops[1].u.imm;
                emit_slot_load(&out, dst, T2_FP_REG, off, ins.width, loc);
            } else if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = ins.ops[1].u.imm;
                emit_slot_store(&out, src, T2_SP, off, ins.width, loc);
            } else if (ins.op == MI_CALL_ADJ_DOWN) {
                int64_t n = ins.ops[0].u.imm;
                if (n > 0) {
                    MInst sp = minst_make(MI_T2_ADJ_SP, 4u, loc);
                    sp.op_count = 1u; sp.ops[0] = mop_imm(-n);
                    PIR_VEC_PUSH(&out, sp);
                }
            } else if (ins.op == MI_CALL_ADJ_UP) {
                int64_t n = ins.ops[0].u.imm;
                if (n > 0) {
                    MInst sp = minst_make(MI_T2_ADJ_SP, 4u, loc);
                    sp.op_count = 1u; sp.ops[0] = mop_imm(n);
                    PIR_VEC_PUSH(&out, sp);
                }
            } else if (ins.op == MI_VA_START) {
                /* Store pointer to first variadic arg into *ap_ptr. */
                MPReg ap = ins.ops[0].u.preg;
                int64_t va_off = (int64_t)(gp_saved_bytes + fp_saved_bytes) +
                                 ((mf->va_fixed_gp < 4u)
                                  ? (int64_t)(mf->va_fixed_gp * 4u)
                                  : (int64_t)(16u + mf->va_fixed_stack_bytes));
                MPReg tmp = (ap == T2_SCRATCH_GP0) ? T2_SCRATCH_GP1 : T2_SCRATCH_GP0;
                emit_add_fp_off(&out, tmp, T2_FP_REG, va_off, loc);
                emit_slot_store(&out, tmp, ap, 0, 4u, loc);
            } else if (ins.op == MI_T2_RET) {
                /* Restore sp = r7 (also reclaims any dynamic alloca_dyn!). */
                if (local_sz > 0u || mf->has_alloca_dyn) {
                    MInst rsp = minst_make(MI_T2_MOV, 4u, loc);
                    rsp.op_count = 2u; rsp.ops[0] = mop_preg_def(T2_SP); rsp.ops[1] = mop_preg_use(T2_FP_REG);
                    PIR_VEC_PUSH(&out, rsp);
                }
                if (!mf->is_variadic && fp_cnt == 0u) {
                    /* Fast single-instruction return: pop.w {..., r7, pc}! */
                    uint32_t ret_mask = (gp_mask & ~(1u << 14u)) | (1u << 15u);
                    MInst pp = minst_make(MI_T2_POP, 4u, loc);
                    pp.op_count = 1u; pp.ops[0] = mop_imm((int64_t)ret_mask);
                    PIR_VEC_PUSH(&out, pp);
                } else {
                    MInst pg = minst_make(MI_T2_POP, 4u, loc);
                    pg.op_count = 1u; pg.ops[0] = mop_imm((int64_t)gp_mask);
                    PIR_VEC_PUSH(&out, pg);
                    if (fp_cnt > 0u) {
                        MInst vp = minst_make(MI_T2_VPOP, 8u, loc);
                        vp.op_count = 2u; vp.ops[0] = mop_imm((int64_t)fp_first); vp.ops[1] = mop_imm((int64_t)fp_cnt);
                        PIR_VEC_PUSH(&out, vp);
                    }
                    if (mf->is_variadic) {
                        MInst sp = minst_make(MI_T2_ADJ_SP, 4u, loc);
                        sp.op_count = 1u; sp.ops[0] = mop_imm(16);
                        PIR_VEC_PUSH(&out, sp);
                    }
                    PIR_VEC_PUSH(&out, ins); /* emits bx lr */
                }
            } else {
                PIR_VEC_PUSH(&out, ins);
            }
        }

        PIR_VEC_FREE(&blk->insts);
        blk->insts = out;
    }

    return true;
}
