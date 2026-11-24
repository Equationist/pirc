/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static bool fits_i32(int64_t v) {
    return v >= INT32_MIN && v <= INT32_MAX;
}

/* Compute dst = base + signed_off into dst (may clobber dst). */
static void emit_x64_lea_off(MInstVec *out, MPReg dst, MPReg base, int64_t off, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            MInst m = minst_make(MI_X64_MOV_RR, 8u, loc);
            m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_preg_use(base);
            PIR_VEC_PUSH(out, m);
        }
        return;
    }
    if (fits_i32(off)) {
        MInst lea = minst_make(MI_X64_LEA, 8u, loc);
        lea.op_count = 3u;
        lea.ops[0] = mop_preg_def(dst);
        lea.ops[1] = mop_preg_use(base);
        lea.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, lea);
        return;
    }
    MInst mi = minst_make(MI_X64_MOV_RI, 8u, loc);
    mi.op_count = 2u; mi.ops[0] = mop_preg_def(dst); mi.ops[1] = mop_imm(off);
    PIR_VEC_PUSH(out, mi);
    MInst ad = minst_make(MI_X64_ADD_RR, 8u, loc);
    ad.op_count = 2u; ad.ops[0] = mop_preg_defuse(dst); ad.ops[1] = mop_preg_use(base);
    PIR_VEC_PUSH(out, ad);
}

static void emit_x64_mem(MInstVec *out, bool is_load, MPReg reg, MPReg base, int64_t off,
                         uint8_t width, bool is_signed, MPReg addr_scratch, uint32_t loc) {
    bool is_fp = x64_is_fp_preg(reg);
    MOpcode mop = is_fp ? (is_load ? MI_X64_MOVX_RM : MI_X64_MOVX_MR)
                        : (is_load ? MI_X64_MOV_RM  : MI_X64_MOV_MR);
    if (fits_i32(off)) {
        MInst m = minst_make(mop, width, loc);
        m.is_signed = is_signed;
        m.op_count  = 3u;
        m.ops[0]    = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
        m.ops[1]    = mop_preg_use(base);
        m.ops[2]    = mop_imm(off);
        PIR_VEC_PUSH(out, m);
        return;
    }
    emit_x64_lea_off(out, addr_scratch, base, off, loc);
    MInst m = minst_make(mop, width, loc);
    m.is_signed = is_signed;
    m.op_count  = 3u;
    m.ops[0]    = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
    m.ops[1]    = mop_preg_use(addr_scratch);
    m.ops[2]    = mop_imm(0);
    PIR_VEC_PUSH(out, m);
}

static void emit_x64_sp_adjust(MInstVec *out, bool is_sub, uint64_t bytes,
                               bool need_chkstk, uint32_t loc) {
    if (bytes == 0u) return;
    if (is_sub && need_chkstk) {
        /* Darwin x86-64 stack probe: size in rax, call ____chkstk_darwin, then sub rsp, rax.
         * ____chkstk_darwin preserves rax and all SysV argument registers (rdi..r9, xmm0..xmm7). */
        MInst mi = minst_make(MI_X64_MOV_RI, 8u, loc);
        mi.op_count = 2u; mi.ops[0] = mop_preg_def(X64_RAX); mi.ops[1] = mop_imm((int64_t)bytes);
        PIR_VEC_PUSH(out, mi);
        MInst call = minst_make(MI_X64_CALL, 8u, loc);
        call.op_count = 1u;
        call.ops[0]   = mop_sym("___chkstk_darwin", 0, true, false, false);
        PIR_VEC_PUSH(out, call);
        MInst sub = minst_make(MI_X64_SUB_RR, 8u, loc);
        sub.op_count = 2u; sub.ops[0] = mop_preg_defuse(X64_RSP); sub.ops[1] = mop_preg_use(X64_RAX);
        PIR_VEC_PUSH(out, sub);
        return;
    }
    if (bytes <= (uint64_t)INT32_MAX) {
        MInst adj = minst_make(is_sub ? MI_X64_SUB_RI : MI_X64_ADD_RI, 8u, loc);
        adj.op_count = 2u;
        adj.ops[0]   = mop_preg_defuse(X64_RSP);
        adj.ops[1]   = mop_imm((int64_t)bytes);
        PIR_VEC_PUSH(out, adj);
        return;
    }
    MInst mi = minst_make(MI_X64_MOV_RI, 8u, loc);
    mi.op_count = 2u; mi.ops[0] = mop_preg_def(X64_SCRATCH_GP0); mi.ops[1] = mop_imm((int64_t)bytes);
    PIR_VEC_PUSH(out, mi);
    MInst adj = minst_make(is_sub ? MI_X64_SUB_RR : MI_X64_ADD_RR, 8u, loc);
    adj.op_count = 2u;
    adj.ops[0]   = mop_preg_defuse(X64_RSP);
    adj.ops[1]   = mop_preg_use(X64_SCRATCH_GP0);
    PIR_VEC_PUSH(out, adj);
}

bool pir_x64_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;
    bool is_darwin = (mf->target->os == PIR_OS_MACOS);
    bool is_sysv_va = (mf->is_variadic && mf->target->os != PIR_OS_WINDOWS);

    /* 1. Collect used callee-saved GP registers (rbx, r12, r13, r14, r15). */
    static const MPReg CS_CAND[5] = { X64_RBX, X64_R12, X64_R13, X64_R14, X64_R15 };
    MPReg cs_gp[8];
    uint32_t n_cs_gp = 0u;
    for (uint32_t i = 0u; i < 5u; ++i) {
        if ((mf->used_callee_gp_mask & (1u << x64_reg_num(CS_CAND[i]))) != 0u) {
            cs_gp[n_cs_gp++] = CS_CAND[i];
        }
    }

    /* 2. Lay out fixed negative offsets below rbp (rbp is 16-byte aligned after `push rbp; mov rbp, rsp`):
     *    - Callee-saved GP regs (8B each)
     *    - [SysV variadic only: 176B register save area (6*8B GP + 8*16B XMM), 16B aligned]
     *    - Local stack slots (alloca.ptr and spill slots), each naturally aligned
     *    - Fixed outgoing stack argument area (when !has_alloca_dyn)
     */
    uint64_t cur = 0u;
    int64_t cs_gp_off[8];
    for (uint32_t i = 0u; i < n_cs_gp; ++i) {
        cur += 8u;
        cs_gp_off[i] = -(int64_t)cur;
    }

    if (is_sysv_va) {
        cur = align_up_u64(cur, 16u);
        cur += 176u; /* 48B GP (rdi,rsi,rdx,rcx,r8,r9) + 128B XMM (xmm0..xmm7) */
        mf->x64_reg_save_off = -(int64_t)cur;
    }

    /* Assign small slots (scalars & spills, <= 64B) closest to rbp first for compact disp8 encoding. */
    for (uint32_t pass = 0u; pass < 2u; ++pass) {
        for (uint32_t s = 0u; s < mf->slots.len; ++s) {
            uint64_t sz = mf->slots.data[s].size;
            if ((pass == 0u) != (sz <= 64u)) continue;
            uint32_t al = mf->slots.data[s].align;
            if (al < 1u) al = 1u;
            cur += sz;
            cur = align_up_u64(cur, (uint64_t)al);
            mf->slots.data[s].fp_offset = -(int64_t)cur;
        }
    }

    if (!mf->has_alloca_dyn && mf->max_out_args_bytes > 0u) {
        cur = align_up_u64(cur, 16u);
        cur += align_up_u64(mf->max_out_args_bytes, 16u);
    }

    mf->local_frame_size = align_up_u64(cur, 16u);
    bool need_chkstk = is_darwin && (mf->local_frame_size >= 4096u);

    /* 3. Rewrite every block: prepend prologue in block 0, expand pseudo-ops, expand epilogue before RET. */
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u) {
            uint32_t ploc = (blk->insts.len > 0u) ? blk->insts.data[0].loc_idx : 0u;

            /* push rbp; mov rbp, rsp (establishes 16B-aligned frame pointer chain) */
            MInst push_fp = minst_make(MI_X64_PUSH, 8u, ploc);
            push_fp.op_count = 1u; push_fp.ops[0] = mop_preg_use(X64_RBP);
            PIR_VEC_PUSH(&out, push_fp);

            MInst set_fp = minst_make(MI_X64_MOV_RSP, 8u, ploc);
            set_fp.aux_id   = 0u;
            set_fp.op_count = 1u; set_fp.ops[0] = mop_preg_def(X64_RBP);
            PIR_VEC_PUSH(&out, set_fp);

            /* Allocate local frame (with ____chkstk_darwin on macOS if >= 4KB) */
            emit_x64_sp_adjust(&out, true, mf->local_frame_size, need_chkstk, ploc);

            /* Save callee-saved GP registers at fixed negative rbp offsets */
            for (uint32_t i = 0u; i < n_cs_gp; ++i) {
                emit_x64_mem(&out, false, cs_gp[i], X64_RBP, cs_gp_off[i],
                             8u, false, X64_SCRATCH_GP0, ploc);
            }

            /* SysV AMD64 variadic prologue: save GP arg regs + XMM0..XMM7 into register save area. */
            if (is_sysv_va) {
                static const MPReg SYSV_GP[6] = {
                    X64_RDI, X64_RSI, X64_RDX, X64_RCX, X64_R8, X64_R9
                };
                for (uint32_t r = 0u; r < 6u; ++r) {
                    emit_x64_mem(&out, false, SYSV_GP[r], X64_RBP,
                                 mf->x64_reg_save_off + (int64_t)(r * 8u),
                                 8u, false, X64_SCRATCH_GP0, ploc);
                }
                for (uint32_t r = 0u; r < 8u; ++r) {
                    emit_x64_mem(&out, false, X64_XMM(r), X64_RBP,
                                 mf->x64_reg_save_off + 48 + (int64_t)(r * 16u),
                                 16u, false, X64_SCRATCH_GP0, ploc);
                }
            }
        }

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            uint32_t loc = ins.loc_idx;

            if (ins.op == MI_NOP) {
                continue;
            }
            if (ins.op == MI_COPY) {
                MPReg d = ins.ops[0].u.preg;
                MPReg s = ins.ops[1].u.preg;
                if (d == s) continue;
                MInst mv = minst_make(x64_is_fp_preg(d) ? MI_X64_MOVX_RR : MI_X64_MOV_RR,
                                      ins.width, loc);
                mv.op_count = 2u; mv.ops[0] = mop_preg_def(d); mv.ops[1] = mop_preg_use(s);
                PIR_VEC_PUSH(&out, mv);
                continue;
            }
            if (ins.op == MI_FRAME_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                uint32_t sid = ins.ops[1].u.slot_id;
                int64_t extra = (ins.op_count >= 3u) ? ins.ops[2].u.imm : 0;
                int64_t off = mf->slots.data[sid].fp_offset + extra;
                emit_x64_lea_off(&out, dst, X64_RBP, off, loc);
                continue;
            }
            if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                uint32_t sid = ins.ops[1].u.slot_id;
                int64_t off = mf->slots.data[sid].fp_offset;
                MPReg scratch = x64_is_fp_preg(dst) ? X64_SCRATCH_GP1 : dst;
                emit_x64_mem(&out, true, dst, X64_RBP, off, ins.width, ins.is_signed, scratch, loc);
                continue;
            }
            if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                uint32_t sid = ins.ops[1].u.slot_id;
                int64_t off = mf->slots.data[sid].fp_offset;
                MPReg scratch = (src == X64_SCRATCH_GP0) ? X64_SCRATCH_GP1 : X64_SCRATCH_GP0;
                emit_x64_mem(&out, false, src, X64_RBP, off, ins.width, false, scratch, loc);
                continue;
            }
            if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = 16 + ins.ops[1].u.imm; /* Above saved rbp + return rip */
                MPReg scratch = x64_is_fp_preg(dst) ? X64_SCRATCH_GP1 : dst;
                emit_x64_mem(&out, true, dst, X64_RBP, off, ins.width, ins.is_signed, scratch, loc);
                continue;
            }
            if (ins.op == MI_INARG_ADDR) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = 16 + ins.ops[1].u.imm;
                emit_x64_lea_off(&out, dst, X64_RBP, off, loc);
                continue;
            }
            if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = ins.ops[1].u.imm; /* Relative to current RSP */
                MPReg scratch = (src == X64_SCRATCH_GP0) ? X64_SCRATCH_GP1 : X64_SCRATCH_GP0;
                emit_x64_mem(&out, false, src, X64_RSP, off, ins.width, false, scratch, loc);
                continue;
            }
            if (ins.op == MI_COPY_OUTARG_MEM) {
                MPReg src_ptr = ins.ops[0].u.preg;
                int64_t dst_off = ins.ops[1].u.imm;
                uint64_t rem = (uint64_t)ins.aux_id;
                MPReg tmp = (src_ptr == X64_SCRATCH_GP0) ? X64_SCRATCH_GP1 : X64_SCRATCH_GP0;
                int64_t cur_off = 0;
                static const uint8_t CHUNKS[4] = { 8u, 4u, 2u, 1u };
                for (uint32_t ci = 0u; ci < 4u && rem > 0u; ++ci) {
                    uint8_t cw = CHUNKS[ci];
                    while (rem >= cw) {
                        emit_x64_mem(&out, true, tmp, src_ptr, cur_off, cw, false, tmp, loc);
                        emit_x64_mem(&out, false, tmp, X64_RSP, dst_off + cur_off, cw, false, tmp, loc);
                        cur_off += cw;
                        rem     -= cw;
                    }
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_DOWN) {
                if (mf->has_alloca_dyn) {
                    uint64_t bytes = align_up_u64((uint64_t)ins.ops[0].u.imm, 16u);
                    emit_x64_sp_adjust(&out, true, bytes, false, loc);
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_UP) {
                if (mf->has_alloca_dyn) {
                    uint64_t bytes = align_up_u64((uint64_t)ins.ops[0].u.imm, 16u);
                    emit_x64_sp_adjust(&out, false, bytes, false, loc);
                }
                continue;
            }
            if (ins.op == MI_VA_START) {
                MPReg ap_ptr = ins.ops[0].u.preg;
                MPReg tmp = (ap_ptr == X64_SCRATCH_GP0) ? X64_SCRATCH_GP1 : X64_SCRATCH_GP0;
                /* SysV AMD64 va_list:
                 *   @0:  gp_offset (i32) = min(va_fixed_gp, 6) * 8
                 *   @4:  fp_offset (i32) = 48 + min(va_fixed_fp, 8) * 16
                 *   @8:  overflow_arg_area (ptr) = rbp + 16 + align_up(va_fixed_stack_bytes, 8)
                 *   @16: reg_save_area (ptr) = rbp + x64_reg_save_off
                 */
                uint32_t gp_off = (mf->va_fixed_gp < 6u ? mf->va_fixed_gp : 6u) * 8u;
                uint32_t fp_off = 48u + (mf->va_fixed_fp < 8u ? mf->va_fixed_fp : 8u) * 16u;
                uint64_t va_stk = align_up_u64(mf->va_fixed_stack_bytes, 8u);

                MInst m0 = minst_make(MI_X64_MOV_RI, 4u, loc);
                m0.op_count = 2u; m0.ops[0] = mop_preg_def(tmp); m0.ops[1] = mop_imm((int64_t)gp_off);
                PIR_VEC_PUSH(&out, m0);
                emit_x64_mem(&out, false, tmp, ap_ptr, 0, 4u, false, tmp, loc);

                MInst m1 = minst_make(MI_X64_MOV_RI, 4u, loc);
                m1.op_count = 2u; m1.ops[0] = mop_preg_def(tmp); m1.ops[1] = mop_imm((int64_t)fp_off);
                PIR_VEC_PUSH(&out, m1);
                emit_x64_mem(&out, false, tmp, ap_ptr, 4, 4u, false, tmp, loc);

                emit_x64_lea_off(&out, tmp, X64_RBP, 16 + (int64_t)va_stk, loc);
                emit_x64_mem(&out, false, tmp, ap_ptr, 8, 8u, false, tmp, loc);

                emit_x64_lea_off(&out, tmp, X64_RBP, mf->x64_reg_save_off, loc);
                emit_x64_mem(&out, false, tmp, ap_ptr, 16, 8u, false, tmp, loc);
                continue;
            }
            if (ins.op == MI_X64_RET) {
                /* Restore callee-saved GP registers */
                for (uint32_t i = n_cs_gp; i-- > 0u; ) {
                    emit_x64_mem(&out, true, cs_gp[i], X64_RBP, cs_gp_off[i],
                                 8u, false, X64_SCRATCH_GP0, loc);
                }
                if (mf->local_frame_size > 0u || mf->has_alloca_dyn) {
                    MInst rst_sp = minst_make(MI_X64_MOV_RSP, 8u, loc);
                    rst_sp.aux_id   = 1u;
                    rst_sp.op_count = 1u;
                    rst_sp.ops[0]   = mop_preg_use(X64_RBP);
                    PIR_VEC_PUSH(&out, rst_sp);
                }
                MInst pop_fp = minst_make(MI_X64_POP, 8u, loc);
                pop_fp.op_count = 1u; pop_fp.ops[0] = mop_preg_def(X64_RBP);
                PIR_VEC_PUSH(&out, pop_fp);

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
