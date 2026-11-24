/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <stdlib.h>
#include <string.h>

static uint64_t align_up_u64(uint64_t v, uint64_t al) {
    return (v + al - 1u) & ~(al - 1u);
}

static void emit_mov_u64(MInstVec *out, MPReg dst, uint64_t val, uint32_t loc) {
    uint16_t chunks[4] = {
        (uint16_t)(val & 0xFFFFu),
        (uint16_t)((val >> 16u) & 0xFFFFu),
        (uint16_t)((val >> 32u) & 0xFFFFu),
        (uint16_t)((val >> 48u) & 0xFFFFu)
    };
    bool first = true;
    for (uint32_t i = 0u; i < 4u; ++i) {
        if (chunks[i] == 0u && !(first && i == 3u)) continue;
        if (first) {
            MInst m = minst_make(MI_A64_MOVZ, 8u, loc);
            m.aux_width = (uint8_t)(i * 16u);
            m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_imm(chunks[i]);
            PIR_VEC_PUSH(out, m);
            first = false;
        } else {
            MInst k = minst_make(MI_A64_MOVK, 8u, loc);
            k.aux_width = (uint8_t)(i * 16u);
            k.op_count = 2u; k.ops[0] = mop_preg_defuse(dst); k.ops[1] = mop_imm(chunks[i]);
            PIR_VEC_PUSH(out, k);
        }
    }
}

/* Compute dst = base + signed_off using at most 1 scratch reg (which may equal dst). */
static void emit_add_signed_off(MInstVec *out, MPReg dst, MPReg base, int64_t off,
                                MPReg scratch, uint32_t loc) {
    if (off == 0) {
        if (dst != base) {
            if (base == A64_SP) {
                MInst m = minst_make(MI_A64_MOV_SP, 8u, loc);
                m.aux_id = 0u; m.op_count = 1u; m.ops[0] = mop_preg_def(dst);
                PIR_VEC_PUSH(out, m);
            } else {
                MInst m = minst_make(MI_A64_MOV_RR, 8u, loc);
                m.op_count = 2u; m.ops[0] = mop_preg_def(dst); m.ops[1] = mop_preg_use(base);
                PIR_VEC_PUSH(out, m);
            }
        }
        return;
    }
    if (off > 0 && off <= 4095) {
        MInst a = minst_make(MI_A64_ADD_RI, 8u, loc);
        a.op_count = 3u; a.ops[0] = mop_preg_def(dst); a.ops[1] = mop_preg_use(base); a.ops[2] = mop_imm(off);
        PIR_VEC_PUSH(out, a);
        return;
    }
    if (off < 0 && -off <= 4095) {
        MInst s = minst_make(MI_A64_SUB_RI, 8u, loc);
        s.op_count = 3u; s.ops[0] = mop_preg_def(dst); s.ops[1] = mop_preg_use(base); s.ops[2] = mop_imm(-off);
        PIR_VEC_PUSH(out, s);
        return;
    }
    uint64_t mag = (off < 0) ? (uint64_t)(-off) : (uint64_t)off;
    emit_mov_u64(out, scratch, mag, loc);
    MInst op = minst_make(off < 0 ? MI_A64_SUB_RR : MI_A64_ADD_RR, 8u, loc);
    op.op_count = 3u; op.ops[0] = mop_preg_def(dst); op.ops[1] = mop_preg_use(base); op.ops[2] = mop_preg_use(scratch);
    PIR_VEC_PUSH(out, op);
}

static bool fits_ldr_str_imm(int64_t off, uint8_t width) {
    if (off >= -256 && off <= 255) return true; /* ldur/stur */
    if (off >= 0 && width > 0u && (off % width) == 0 && (off / width) <= 4095) return true;
    return false;
}

static void emit_mem_at_base_off(MInstVec *out, bool is_load, MPReg reg,
                                 MPReg base, int64_t off, uint8_t width,
                                 bool is_signed, MPReg addr_scratch, uint32_t loc) {
    if (fits_ldr_str_imm(off, width)) {
        MInst m = minst_make(is_load ? MI_A64_LDR : MI_A64_STR, width, loc);
        m.is_signed = is_signed;
        m.op_count  = 3u;
        m.ops[0]    = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
        m.ops[1]    = mop_preg_use(base);
        m.ops[2]    = mop_imm(off);
        PIR_VEC_PUSH(out, m);
        return;
    }
    emit_add_signed_off(out, addr_scratch, base, off, addr_scratch, loc);
    MInst m = minst_make(is_load ? MI_A64_LDR : MI_A64_STR, width, loc);
    m.is_signed = is_signed;
    m.op_count  = 3u;
    m.ops[0]    = is_load ? mop_preg_def(reg) : mop_preg_use(reg);
    m.ops[1]    = mop_preg_use(addr_scratch);
    m.ops[2]    = mop_imm(0);
    PIR_VEC_PUSH(out, m);
}

static void emit_sp_adjust(MInstVec *out, bool is_sub, uint64_t bytes,
                           bool need_chkstk, uint32_t loc) {
    if (bytes == 0u) return;
    if (is_sub && need_chkstk) {
        /* Darwin ARM64 stack probe: size in x9, call ___chkstk_darwin, then sub sp, sp, x9. */
        emit_mov_u64(out, A64_GP(9u), bytes, loc);
        MInst bl = minst_make(MI_A64_BL, 8u, loc);
        bl.op_count = 1u;
        bl.ops[0]   = mop_sym("__chkstk_darwin", 0, true, false, false);
        PIR_VEC_PUSH(out, bl);
        MInst sub = minst_make(MI_A64_SUB_RR, 8u, loc);
        sub.op_count = 3u;
        sub.ops[0] = mop_preg_def(A64_SP);
        sub.ops[1] = mop_preg_use(A64_SP);
        sub.ops[2] = mop_preg_use(A64_GP(9u));
        PIR_VEC_PUSH(out, sub);
        return;
    }
    if (bytes <= 4095u) {
        MInst adj = minst_make(is_sub ? MI_A64_SUB_RI : MI_A64_ADD_RI, 8u, loc);
        adj.op_count = 3u;
        adj.ops[0] = mop_preg_def(A64_SP);
        adj.ops[1] = mop_preg_use(A64_SP);
        adj.ops[2] = mop_imm((int64_t)bytes);
        PIR_VEC_PUSH(out, adj);
        return;
    }
    /* Materialize > 4095B offset into x16 */
    emit_mov_u64(out, A64_SCRATCH_GP1, bytes, loc);
    MInst adj = minst_make(is_sub ? MI_A64_SUB_RR : MI_A64_ADD_RR, 8u, loc);
    adj.op_count = 3u;
    adj.ops[0] = mop_preg_def(A64_SP);
    adj.ops[1] = mop_preg_use(A64_SP);
    adj.ops[2] = mop_preg_use(A64_SCRATCH_GP1);
    PIR_VEC_PUSH(out, adj);
}

static bool a64_is_callee_saved_gp(MPReg r) {
    return !a64_is_fp_preg(r) && r >= A64_GP(19u) && r <= A64_GP(28u);
}

static bool a64_is_caller_saved_gp(MPReg r) {
    return !a64_is_fp_preg(r) && r <= A64_GP(14u);
}

static bool a64_is_safe_fastpath_alu(MOpcode op) {
    return op == MI_COPY       || op == MI_A64_MOV_RR ||
           op == MI_A64_MOVZ   || op == MI_A64_MOVN   || op == MI_A64_MOVK   ||
           op == MI_A64_ADD_RR || op == MI_A64_ADD_RI ||
           op == MI_A64_SUB_RR || op == MI_A64_SUB_RI ||
           op == MI_A64_AND_RR || op == MI_A64_ORR_RR || op == MI_A64_EOR_RR ||
           op == MI_A64_AND_RI || op == MI_A64_ORR_RI || op == MI_A64_EOR_RI ||
           op == MI_A64_LSL_RI || op == MI_A64_LSR_RI || op == MI_A64_ASR_RI ||
           op == MI_A64_SXTB   || op == MI_A64_SXTH   || op == MI_A64_SXTW   ||
           op == MI_A64_UXTB   || op == MI_A64_UXTH   ||
           op == MI_A64_CMP_RR || op == MI_A64_CMP_RI || op == MI_A64_TST_RR;
}

static bool a64_rewrite_fastpath_inst(MInst *ins, const MPReg cs_map[32],
                                      const uint8_t cs_map_w[32],
                                      uint32_t clobbered_mask,
                                      uint32_t *out_def_mask) {
    uint32_t def_mask = 0u;
    for (uint32_t k = 0u; k < ins->op_count; ++k) {
        MOperand *op = &ins->ops[k];
        if (op->kind == MOP_IMM || op->kind == MOP_COND || op->kind == MOP_BLOCK) {
            continue;
        }
        if (op->kind != MOP_PREG) return false;
        MPReg r = op->u.preg;
        if (a64_is_fp_preg(r)) return false;

        if (op->is_use && op->is_def) {
            if (!a64_is_caller_saved_gp(r)) return false;
            def_mask |= (1u << a64_reg_num(r));
        } else if (op->is_use) {
            if (a64_is_callee_saved_gp(r)) {
                uint32_t rn = a64_reg_num(r);
                MPReg mapped = cs_map[rn];
                if (mapped == MREG_NONE || ins->width > cs_map_w[rn]) return false;
                if ((clobbered_mask & (1u << a64_reg_num(mapped))) != 0u) return false;
                op->u.preg = mapped;
            } else if (!a64_is_caller_saved_gp(r)) {
                return false;
            }
        } else if (op->is_def) {
            if (!a64_is_caller_saved_gp(r)) return false;
            def_mask |= (1u << a64_reg_num(r));
        }
    }
    if (ins->op == MI_COPY) {
        ins->op = MI_A64_MOV_RR;
    }
    if (out_def_mask) *out_def_mask = def_mask;
    return true;
}

static bool a64_collect_leaf_ret_path(const MFunc *mf, uint32_t start_bid,
                                      const MPReg cs_map[32], const uint8_t cs_map_w[32],
                                      MInst out_insts[8], uint32_t *out_len) {
    uint32_t bid = start_bid;
    uint32_t n_out = 0u;
    uint32_t clobbered = 0u;

    for (uint32_t hop = 0u; hop < 2u; ++hop) {
        if (bid >= mf->blocks.len) return false;
        const MBlock *blk = &mf->blocks.data[bid];
        bool jumped = false;
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MInst ins = blk->insts.data[j];
            if (ins.op == MI_NOP) continue;
            if (ins.op == MI_A64_RET) {
                if (n_out >= 8u) return false;
                out_insts[n_out++] = ins;
                *out_len = n_out;
                return true;
            }
            if (ins.op == MI_A64_B && j + 1u == blk->insts.len) {
                uint32_t next_bid = ins.ops[0].u.block_id;
                if (next_bid == bid) return false;
                bid = next_bid;
                jumped = true;
                break;
            }
            if (!a64_is_safe_fastpath_alu(ins.op)) return false;
            uint32_t def_m = 0u;
            if (!a64_rewrite_fastpath_inst(&ins, cs_map, cs_map_w, clobbered, &def_m)) {
                return false;
            }
            clobbered |= def_m;
            if (ins.op == MI_A64_MOV_RR && ins.ops[0].u.preg == ins.ops[1].u.preg) {
                continue;
            }
            if (n_out >= 7u) return false;
            out_insts[n_out++] = ins;
        }
        if (!jumped) return false;
    }
    return false;
}

static void emit_arm64_prologue(MInstVec *out, const MFunc *mf,
                                const MPReg *cs_gp, uint32_t n_cs_gp,
                                const int64_t *cs_gp_off, uint32_t n_gp_pairs,
                                const MPReg *cs_fp, uint32_t n_cs_fp,
                                const int64_t *cs_fp_off, uint32_t n_fp_pairs,
                                bool need_chkstk, bool is_linux_va, uint32_t ploc) {
    /* Always maintain Darwin/AAPCS64 x29/x30 frame pointer chain:
     *   stp x29, x30, [sp, #-16]!
     *   mov x29, sp
     */
    MInst stp_fp = minst_make(MI_A64_STP, 8u, ploc);
    stp_fp.addr_mode = MADDR_PRE_INDEX;
    stp_fp.op_count  = 4u;
    stp_fp.ops[0] = mop_preg_use(A64_FP_REG);
    stp_fp.ops[1] = mop_preg_use(A64_LR_REG);
    stp_fp.ops[2] = mop_preg_defuse(A64_SP);
    stp_fp.ops[3] = mop_imm(-16);
    PIR_VEC_PUSH(out, stp_fp);

    MInst set_fp = minst_make(MI_A64_MOV_SP, 8u, ploc);
    set_fp.aux_id   = 0u;
    set_fp.op_count = 1u;
    set_fp.ops[0]   = mop_preg_def(A64_FP_REG);
    PIR_VEC_PUSH(out, set_fp);

    /* Allocate local frame (with ___chkstk_darwin if >= 16KB on macOS). */
    emit_sp_adjust(out, true, mf->local_frame_size, need_chkstk, ploc);

    /* Save callee-saved GP pairs right below x29 (always within stp range!). */
    for (uint32_t i = 0u; i < n_gp_pairs; ++i) {
        MPReg r0 = cs_gp[i * 2u];
        if (i * 2u + 1u < n_cs_gp) {
            MPReg r1 = cs_gp[i * 2u + 1u];
            MInst stp = minst_make(MI_A64_STP, 8u, ploc);
            stp.addr_mode = MADDR_OFFSET;
            stp.op_count  = 4u;
            stp.ops[0] = mop_preg_use(r0);
            stp.ops[1] = mop_preg_use(r1);
            stp.ops[2] = mop_preg_use(A64_FP_REG);
            stp.ops[3] = mop_imm(cs_gp_off[i]);
            PIR_VEC_PUSH(out, stp);
        } else {
            emit_mem_at_base_off(out, false, r0, A64_FP_REG, cs_gp_off[i],
                                 8u, false, A64_SCRATCH_GP1, ploc);
        }
    }

    /* Save callee-saved FP pairs (d8..d15, 8 bytes each). */
    for (uint32_t i = 0u; i < n_fp_pairs; ++i) {
        MPReg r0 = cs_fp[i * 2u];
        if (i * 2u + 1u < n_cs_fp) {
            MPReg r1 = cs_fp[i * 2u + 1u];
            MInst stp = minst_make(MI_A64_STP, 8u, ploc);
            stp.addr_mode = MADDR_OFFSET;
            stp.op_count  = 4u;
            stp.ops[0] = mop_preg_use(r0);
            stp.ops[1] = mop_preg_use(r1);
            stp.ops[2] = mop_preg_use(A64_FP_REG);
            stp.ops[3] = mop_imm(cs_fp_off[i]);
            PIR_VEC_PUSH(out, stp);
        } else {
            emit_mem_at_base_off(out, false, r0, A64_FP_REG, cs_fp_off[i],
                                 8u, false, A64_SCRATCH_GP1, ploc);
        }
    }

    /* Linux AAPCS64 variadic prologue: save x0..x7 and q0..q7 into reg save area. */
    if (is_linux_va) {
        for (uint32_t r = mf->va_fixed_gp; r < 8u; ++r) {
            int64_t off = mf->linux_gr_top_off - (int64_t)((8u - r) * 8u);
            emit_mem_at_base_off(out, false, A64_GP(r), A64_FP_REG, off,
                                 8u, false, A64_SCRATCH_GP1, ploc);
        }
        for (uint32_t r = mf->va_fixed_fp; r < 8u; ++r) {
            int64_t off = mf->linux_vr_top_off - (int64_t)((8u - r) * 16u);
            emit_mem_at_base_off(out, false, A64_FP(r), A64_FP_REG, off,
                                 16u, false, A64_SCRATCH_GP1, ploc);
        }
    }
}

static MCond a64_invert_cond(MCond c) {
    switch (c) {
        case MCOND_EQ: return MCOND_NE;
        case MCOND_NE: return MCOND_EQ;
        case MCOND_CS: return MCOND_CC;
        case MCOND_CC: return MCOND_CS;
        case MCOND_MI: return MCOND_PL;
        case MCOND_PL: return MCOND_MI;
        case MCOND_VS: return MCOND_VC;
        case MCOND_VC: return MCOND_VS;
        case MCOND_HI: return MCOND_LS;
        case MCOND_LS: return MCOND_HI;
        case MCOND_GE: return MCOND_LT;
        case MCOND_LT: return MCOND_GE;
        case MCOND_GT: return MCOND_LE;
        case MCOND_LE: return MCOND_GT;
        case MCOND_P:  return MCOND_NP;
        case MCOND_NP: return MCOND_P;
    }
    return MCOND_NE;
}

static uint32_t a64_count_block_refs(const MFunc *mf, uint32_t target_bid,
                                     uint32_t skip_bid0, uint32_t skip_bid1) {
    uint32_t cnt = 0u;
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        if (b == skip_bid0 || b == skip_bid1) continue;
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            for (uint32_t k = 0u; k < ins->op_count; ++k) {
                if (ins->ops[k].kind == MOP_BLOCK && ins->ops[k].u.block_id == target_bid) {
                    cnt++;
                }
            }
        }
    }
    for (uint32_t jt = 0u; jt < mf->jtables.len; ++jt) {
        for (uint32_t k = 0u; k < mf->jtables.data[jt].num_entries; ++k) {
            if (mf->jtables.data[jt].targets[k] == target_bid) cnt++;
        }
    }
    return cnt;
}

/* Simple, sound ARM64 shrink-wrapping: when entry block (or its immediate jump target)
 * performs a pure caller-saved guard branching to a leaf early-return path (e.g. recursive
 * base cases in Fib/Tak/QSort), inline the epilogue-free leaf return directly into block 0
 * (zero taken branches on base case!) and attach the prologue + deferred arg copies directly
 * to slow_target (or a trampoline if slow_target has other predecessors). */
static bool try_arm64_shrink_wrap(MFunc *mf,
                                  const MPReg *cs_gp, uint32_t n_cs_gp,
                                  const int64_t *cs_gp_off, uint32_t n_gp_pairs,
                                  const MPReg *cs_fp, uint32_t n_cs_fp,
                                  const int64_t *cs_fp_off, uint32_t n_fp_pairs,
                                  bool need_chkstk, bool is_linux_va,
                                  uint32_t *out_prol_bid, bool *out_prol_inlined) {
    if (mf->blocks.len < 2u || mf->is_variadic || is_linux_va || need_chkstk || mf->has_alloca_dyn) {
        return false;
    }
    if (a64_count_block_refs(mf, 0u, MBLOCK_NONE, MBLOCK_NONE) != 0u) return false;

    MPReg   cs_map[32];
    uint8_t cs_map_w[32];
    for (uint32_t r = 0u; r < 32u; ++r) { cs_map[r] = MREG_NONE; cs_map_w[r] = 0u; }

    MInst    deferred[10];
    uint32_t n_deferred = 0u;
    uint32_t src_pinned_mask = 0u;

    MInst    prefix[12];
    uint32_t n_prefix = 0u;

    const MBlock *blk0 = &mf->blocks.data[0];
    uint32_t test_bid = 0u;
    bool found_branch = false;
    MInst br_inst;
    memset(&br_inst, 0, sizeof(br_inst));
    uint32_t t_bid = MBLOCK_NONE;
    uint32_t f_bid = MBLOCK_NONE;
    uint32_t ploc = (blk0->insts.len > 0u) ? blk0->insts.data[0].loc_idx : 0u;

    for (uint32_t j = 0u; j < blk0->insts.len; ++j) {
        MInst ins = blk0->insts.data[j];
        if (ins.op == MI_NOP) continue;

        /* Defer callee-saved entry-arg copies (`mov x19, x0`) into the slow-path prologue. */
        if ((ins.op == MI_COPY || ins.op == MI_A64_MOV_RR) && ins.op_count == 2u &&
            ins.ops[0].kind == MOP_PREG && ins.ops[1].kind == MOP_PREG &&
            a64_is_callee_saved_gp(ins.ops[0].u.preg) &&
            a64_is_caller_saved_gp(ins.ops[1].u.preg)) {
            uint32_t dn = a64_reg_num(ins.ops[0].u.preg);
            if (cs_map[dn] != MREG_NONE || n_deferred >= 10u) return false;
            cs_map[dn]   = ins.ops[1].u.preg;
            cs_map_w[dn] = ins.width;
            src_pinned_mask |= (1u << a64_reg_num(ins.ops[1].u.preg));
            if (ins.op == MI_COPY) ins.op = MI_A64_MOV_RR;
            deferred[n_deferred++] = ins;
            continue;
        }

        if (ins.op == MI_A64_B && j + 1u == blk0->insts.len) {
            test_bid = ins.ops[0].u.block_id;
            if (test_bid == 0u || test_bid >= mf->blocks.len) return false;
            break;
        }

        if ((ins.op == MI_A64_BCOND || ins.op == MI_A64_CBZ || ins.op == MI_A64_CBNZ) &&
            j + 2u == blk0->insts.len && blk0->insts.data[j + 1u].op == MI_A64_B) {
            uint32_t br_def = 0u;
            if (!a64_rewrite_fastpath_inst(&ins, cs_map, cs_map_w, 0u, &br_def)) return false;
            br_inst = ins;
            t_bid = ins.ops[1].u.block_id;
            f_bid = blk0->insts.data[j + 1u].ops[0].u.block_id;
            found_branch = true;
            break;
        }

        if (!a64_is_safe_fastpath_alu(ins.op) || n_prefix >= 12u) return false;
        uint32_t def_m = 0u;
        if (!a64_rewrite_fastpath_inst(&ins, cs_map, cs_map_w, 0u, &def_m)) return false;
        if ((def_m & src_pinned_mask) != 0u) return false;
        if (ins.op == MI_A64_MOV_RR && ins.ops[0].u.preg == ins.ops[1].u.preg) continue;
        prefix[n_prefix++] = ins;
    }

    if (!found_branch) {
        if (test_bid == 0u || test_bid >= mf->blocks.len) return false;
        const MBlock *tblk = &mf->blocks.data[test_bid];
        if (tblk->insts.len < 2u || tblk->insts.len > 6u) return false;
        for (uint32_t j = 0u; j < tblk->insts.len; ++j) {
            MInst ins = tblk->insts.data[j];
            if (ins.op == MI_NOP) continue;
            if ((ins.op == MI_A64_BCOND || ins.op == MI_A64_CBZ || ins.op == MI_A64_CBNZ) &&
                j + 2u == tblk->insts.len && tblk->insts.data[j + 1u].op == MI_A64_B) {
                uint32_t br_def = 0u;
                if (!a64_rewrite_fastpath_inst(&ins, cs_map, cs_map_w, 0u, &br_def)) return false;
                br_inst = ins;
                t_bid = ins.ops[1].u.block_id;
                f_bid = tblk->insts.data[j + 1u].ops[0].u.block_id;
                found_branch = true;
                break;
            }
            if (!a64_is_safe_fastpath_alu(ins.op) || n_prefix >= 12u) return false;
            uint32_t def_m = 0u;
            if (!a64_rewrite_fastpath_inst(&ins, cs_map, cs_map_w, 0u, &def_m)) return false;
            if ((def_m & src_pinned_mask) != 0u) return false;
            if (ins.op == MI_A64_MOV_RR && ins.ops[0].u.preg == ins.ops[1].u.preg) continue;
            prefix[n_prefix++] = ins;
        }
    }

    if (!found_branch || t_bid >= mf->blocks.len || f_bid >= mf->blocks.len) return false;
    if (t_bid == 0u || f_bid == 0u) return false;

    MInst    t_leaf_insts[8], f_leaf_insts[8];
    uint32_t t_leaf_len = 0u, f_leaf_len = 0u;
    bool t_is_leaf = a64_collect_leaf_ret_path(mf, t_bid, cs_map, cs_map_w, t_leaf_insts, &t_leaf_len);
    bool f_is_leaf = a64_collect_leaf_ret_path(mf, f_bid, cs_map, cs_map_w, f_leaf_insts, &f_leaf_len);
    if (t_is_leaf == f_is_leaf) return false; /* exactly one side must be the leaf early return */

    uint32_t slow_target    = t_is_leaf ? f_bid : t_bid;
    const MInst *leaf_insts = t_is_leaf ? t_leaf_insts : f_leaf_insts;
    uint32_t     leaf_len   = t_is_leaf ? t_leaf_len   : f_leaf_len;

    /* Determine whether test_bid becomes dead after bypassing it from block 0,
     * and whether slow_target has zero other predecessors so we can prepend the prologue directly! */
    bool test_becomes_dead = (test_bid != 0u &&
                              a64_count_block_refs(mf, test_bid, 0u, MBLOCK_NONE) == 0u);
    uint32_t skip1 = test_becomes_dead ? test_bid : MBLOCK_NONE;
    bool slow_single_pred = (slow_target != test_bid &&
                             a64_count_block_refs(mf, slow_target, 0u, skip1) == 0u);

    if (test_becomes_dead) {
        /* Turn dead test_bid into a 1-inst jump trampoline so arm64_asm omits it completely. */
        MInstVec empty_jmp = {0};
        MInst j = minst_make(MI_A64_B, 4u, ploc);
        j.op_count = 1u; j.ops[0] = mop_block(slow_target);
        PIR_VEC_PUSH(&empty_jmp, j);
        PIR_VEC_FREE(&mf->blocks.data[test_bid].insts);
        mf->blocks.data[test_bid].insts = empty_jmp;
    }

    uint32_t prol_target = slow_target;
    if (slow_single_pred) {
        /* Prepend prologue + deferred copies directly to slow_target (zero extra branch!). */
        MInstVec pvec = {0};
        emit_arm64_prologue(&pvec, mf, cs_gp, n_cs_gp, cs_gp_off, n_gp_pairs,
                            cs_fp, n_cs_fp, cs_fp_off, n_fp_pairs, need_chkstk, is_linux_va, ploc);
        for (uint32_t i = 0u; i < n_deferred; ++i) {
            PIR_VEC_PUSH(&pvec, deferred[i]);
        }
        MBlock *sblk = &mf->blocks.data[slow_target];
        for (uint32_t i = 0u; i < sblk->insts.len; ++i) {
            PIR_VEC_PUSH(&pvec, sblk->insts.data[i]);
        }
        PIR_VEC_FREE(&sblk->insts);
        sblk->insts = pvec;
        *out_prol_bid     = slow_target;
        *out_prol_inlined = true;
    } else {
        uint32_t prol_bid = mf->blocks.len;
        MBlock prol_blk;
        memset(&prol_blk, 0, sizeof(prol_blk));
        prol_blk.id = prol_bid;
        emit_arm64_prologue(&prol_blk.insts, mf, cs_gp, n_cs_gp, cs_gp_off, n_gp_pairs,
                            cs_fp, n_cs_fp, cs_fp_off, n_fp_pairs, need_chkstk, is_linux_va, ploc);
        for (uint32_t i = 0u; i < n_deferred; ++i) {
            PIR_VEC_PUSH(&prol_blk.insts, deferred[i]);
        }
        MInst jmp_slow = minst_make(MI_A64_B, 4u, ploc);
        jmp_slow.op_count = 1u;
        jmp_slow.ops[0]   = mop_block(slow_target);
        PIR_VEC_PUSH(&prol_blk.insts, jmp_slow);
        PIR_VEC_PUSH(&mf->blocks, prol_blk);
        prol_target       = prol_bid;
        *out_prol_bid     = prol_bid;
        *out_prol_inlined = false;
    }

    /* Rewrite block 0: guard -> conditional branch to prol_target; fallthrough to inline leaf ret! */
    MInstVec new0 = {0};
    for (uint32_t i = 0u; i < n_prefix; ++i) {
        PIR_VEC_PUSH(&new0, prefix[i]);
    }
    if (t_is_leaf) {
        if (br_inst.op == MI_A64_BCOND) {
            br_inst.ops[0].u.cond = a64_invert_cond(br_inst.ops[0].u.cond);
        } else if (br_inst.op == MI_A64_CBZ) {
            br_inst.op = MI_A64_CBNZ;
        } else if (br_inst.op == MI_A64_CBNZ) {
            br_inst.op = MI_A64_CBZ;
        }
    }
    br_inst.ops[1] = mop_block(prol_target);
    PIR_VEC_PUSH(&new0, br_inst);
    for (uint32_t i = 0u; i < leaf_len; ++i) {
        PIR_VEC_PUSH(&new0, leaf_insts[i]);
    }

    PIR_VEC_FREE(&mf->blocks.data[0].insts);
    mf->blocks.data[0].insts = new0;
    return true;
}

bool pir_arm64_frame_lower(MFunc *mf, PirDiag *diag) {
    (void)diag;
    bool is_darwin = (mf->target->os == PIR_OS_MACOS);
    bool is_linux_va = (mf->is_variadic && !is_darwin);

    /* 1. Collect used callee-saved GP (x19..x28) and FP (d8..d15) registers. */
    MPReg cs_gp[16];
    uint32_t n_cs_gp = 0u;
    for (uint32_t r = 19u; r <= 28u; ++r) {
        if ((mf->used_callee_gp_mask & (1u << r)) != 0u) {
            cs_gp[n_cs_gp++] = A64_GP(r);
        }
    }
    MPReg cs_fp[16];
    uint32_t n_cs_fp = 0u;
    for (uint32_t r = 8u; r <= 15u; ++r) {
        if ((mf->used_callee_fp_mask & (1u << r)) != 0u) {
            cs_fp[n_cs_fp++] = A64_FP(r);
        }
    }

    /* 2. Lay out fixed negative offsets below x29 (FP):
     *    - Callee-saved GP pairs (16B per pair, 8B per lone reg rounded to 16B)
     *    - Callee-saved FP pairs (16B per pair, 8B per lone reg rounded to 16B)
     *    - [Linux variadic only: 64B GR save + 128B VR save = 192B]
     *    - Local slots (alloca.ptr and spill slots), each aligned cleanly!
     *    - Fixed outgoing stack argument area (only when !has_alloca_dyn)
     */
    uint64_t cur = 0u;

    int64_t cs_gp_off[8];
    uint32_t n_gp_pairs = (n_cs_gp + 1u) / 2u;
    for (uint32_t i = 0u; i < n_gp_pairs; ++i) {
        cur += 16u;
        cs_gp_off[i] = -(int64_t)cur;
    }

    int64_t cs_fp_off[8];
    uint32_t n_fp_pairs = (n_cs_fp + 1u) / 2u;
    for (uint32_t i = 0u; i < n_fp_pairs; ++i) {
        cur += 16u;
        cs_fp_off[i] = -(int64_t)cur;
    }

    if (is_linux_va) {
        cur = align_up_u64(cur, 16u);
        mf->linux_gr_top_off = -(int64_t)cur;
        cur += 64u; /* x0..x7 save area: [gr_top - 64 .. gr_top) */
        mf->linux_vr_top_off = -(int64_t)cur;
        cur += 128u; /* q0..q7 save area: [vr_top - 128 .. vr_top) */
    }

    /* Assign small slots (scalars & spills, <= 64B) closest to x29 first so they stay
     * within 1-instruction ldur/stur [-256, 255] range even alongside large local arrays. */
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

    /* Reserve fixed outgoing call arg space below locals if no dynamic alloca. */
    if (!mf->has_alloca_dyn && mf->max_out_args_bytes > 0u) {
        cur = align_up_u64(cur, 16u);
        cur += align_up_u64(mf->max_out_args_bytes, 16u);
    }

    mf->local_frame_size = align_up_u64(cur, 16u);
    bool need_chkstk = is_darwin && (mf->local_frame_size >= 16384u);

    /* Attempt shrink-wrapping so base-case / fast-path returns skip the frame prologue & epilogue. */
    uint32_t sw_prol_bid = MBLOCK_NONE;
    bool sw_prol_inlined = false;
    bool shrink_wrapped = try_arm64_shrink_wrap(mf, cs_gp, n_cs_gp, cs_gp_off, n_gp_pairs,
                                                cs_fp, n_cs_fp, cs_fp_off, n_fp_pairs,
                                                need_chkstk, is_linux_va,
                                                &sw_prol_bid, &sw_prol_inlined);

    /* 3. Rewrite every block: prepend prologue in block 0 (unless shrink-wrapped), expand pseudo-ops, expand epilogue before RET. */
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        if (shrink_wrapped && (b == 0u || (!sw_prol_inlined && b == sw_prol_bid))) continue;
        MBlock *blk = &mf->blocks.data[b];
        MInstVec out = {0};

        if (b == 0u && !shrink_wrapped) {
            uint32_t ploc = (blk->insts.len > 0u) ? blk->insts.data[0].loc_idx : 0u;
            emit_arm64_prologue(&out, mf, cs_gp, n_cs_gp, cs_gp_off, n_gp_pairs,
                                cs_fp, n_cs_fp, cs_fp_off, n_fp_pairs,
                                need_chkstk, is_linux_va, ploc);
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
                MInst mv = minst_make(a64_is_fp_preg(d) ? MI_A64_FMOV_RR : MI_A64_MOV_RR,
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
                /* Compute into dst directly (scratch = dst is safe when dst != x29). */
                emit_add_signed_off(&out, dst, A64_FP_REG, off, dst, loc);
                continue;
            }
            if (ins.op == MI_LOAD_SLOT) {
                MPReg dst = ins.ops[0].u.preg;
                uint32_t sid = ins.ops[1].u.slot_id;
                int64_t off = mf->slots.data[sid].fp_offset;
                MPReg scratch = a64_is_fp_preg(dst) ? A64_SCRATCH_GP1 : dst;
                emit_mem_at_base_off(&out, true, dst, A64_FP_REG, off, ins.width,
                                     ins.is_signed, scratch, loc);
                continue;
            }
            if (ins.op == MI_STORE_SLOT) {
                MPReg src = ins.ops[0].u.preg;
                uint32_t sid = ins.ops[1].u.slot_id;
                int64_t off = mf->slots.data[sid].fp_offset;
                MPReg scratch = (src == A64_SCRATCH_GP1) ? A64_SCRATCH_GP2 : A64_SCRATCH_GP1;
                emit_mem_at_base_off(&out, false, src, A64_FP_REG, off, ins.width,
                                     false, scratch, loc);
                continue;
            }
            if (ins.op == MI_LOAD_INARG) {
                MPReg dst = ins.ops[0].u.preg;
                int64_t off = 16 + ins.ops[1].u.imm; /* Above saved (x29, x30) */
                MPReg scratch = a64_is_fp_preg(dst) ? A64_SCRATCH_GP1 : dst;
                emit_mem_at_base_off(&out, true, dst, A64_FP_REG, off, ins.width,
                                     ins.is_signed, scratch, loc);
                continue;
            }
            if (ins.op == MI_STORE_OUTARG) {
                MPReg src = ins.ops[0].u.preg;
                int64_t off = ins.ops[1].u.imm; /* Relative to current SP */
                MPReg scratch = (src == A64_SCRATCH_GP1) ? A64_SCRATCH_GP2 : A64_SCRATCH_GP1;
                emit_mem_at_base_off(&out, false, src, A64_SP, off, ins.width,
                                     false, scratch, loc);
                continue;
            }
            if (ins.op == MI_CALL_ADJ_DOWN) {
                if (mf->has_alloca_dyn) {
                    uint64_t bytes = align_up_u64((uint64_t)ins.ops[0].u.imm, 16u);
                    emit_sp_adjust(&out, true, bytes, false, loc);
                }
                continue;
            }
            if (ins.op == MI_CALL_ADJ_UP) {
                if (mf->has_alloca_dyn) {
                    uint64_t bytes = align_up_u64((uint64_t)ins.ops[0].u.imm, 16u);
                    emit_sp_adjust(&out, false, bytes, false, loc);
                }
                continue;
            }
            if (ins.op == MI_VA_START) {
                MPReg ap_ptr = ins.ops[0].u.preg;
                MPReg tmp = (ap_ptr == A64_SCRATCH_GP1) ? A64_SCRATCH_GP2 : A64_SCRATCH_GP1;
                uint64_t va_off = align_up_u64(mf->va_fixed_stack_bytes, 8u);
                if (is_darwin) {
                    /* Darwin ARM64 va_list is a flat 8B char* cursor pointing at caller_sp + va_off */
                    emit_add_signed_off(&out, tmp, A64_FP_REG, 16 + (int64_t)va_off, tmp, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 0, 8u, false, tmp, loc);
                } else {
                    /* Linux AAPCS64 va_list:
                     *   @0:  __stack  = x29 + 16 + va_off
                     *   @8:  __gr_top = x29 + linux_gr_top_off
                     *   @16: __vr_top = x29 + linux_vr_top_off
                     *   @24: __gr_offs = -(8 - va_fixed_gp) * 8
                     *   @28: __vr_offs = -(8 - va_fixed_fp) * 16
                     */
                    emit_add_signed_off(&out, tmp, A64_FP_REG, 16 + (int64_t)va_off, tmp, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 0, 8u, false, tmp, loc);

                    emit_add_signed_off(&out, tmp, A64_FP_REG, mf->linux_gr_top_off, tmp, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 8, 8u, false, tmp, loc);

                    emit_add_signed_off(&out, tmp, A64_FP_REG, mf->linux_vr_top_off, tmp, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 16, 8u, false, tmp, loc);

                    int32_t gr_offs = -(int32_t)((8u - (mf->va_fixed_gp < 8u ? mf->va_fixed_gp : 8u)) * 8u);
                    emit_mov_u64(&out, tmp, (uint64_t)(uint32_t)gr_offs, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 24, 4u, false, tmp, loc);

                    int32_t vr_offs = -(int32_t)((8u - (mf->va_fixed_fp < 8u ? mf->va_fixed_fp : 8u)) * 16u);
                    emit_mov_u64(&out, tmp, (uint64_t)(uint32_t)vr_offs, loc);
                    emit_mem_at_base_off(&out, false, tmp, ap_ptr, 28, 4u, false, tmp, loc);
                }
                continue;
            }
            if (ins.op == MI_A64_RET) {
                /* Epilogue: restore callee-saved FP & GP pairs, mov sp, x29, ldp x29, x30, [sp], #16, ret */
                for (uint32_t i = n_fp_pairs; i-- > 0u; ) {
                    MPReg r0 = cs_fp[i * 2u];
                    if (i * 2u + 1u < n_cs_fp) {
                        MPReg r1 = cs_fp[i * 2u + 1u];
                        MInst ldp = minst_make(MI_A64_LDP, 8u, loc);
                        ldp.addr_mode = MADDR_OFFSET;
                        ldp.op_count  = 4u;
                        ldp.ops[0] = mop_preg_def(r0);
                        ldp.ops[1] = mop_preg_def(r1);
                        ldp.ops[2] = mop_preg_use(A64_FP_REG);
                        ldp.ops[3] = mop_imm(cs_fp_off[i]);
                        PIR_VEC_PUSH(&out, ldp);
                    } else {
                        emit_mem_at_base_off(&out, true, r0, A64_FP_REG, cs_fp_off[i],
                                             8u, false, A64_SCRATCH_GP1, loc);
                    }
                }
                for (uint32_t i = n_gp_pairs; i-- > 0u; ) {
                    MPReg r0 = cs_gp[i * 2u];
                    if (i * 2u + 1u < n_cs_gp) {
                        MPReg r1 = cs_gp[i * 2u + 1u];
                        MInst ldp = minst_make(MI_A64_LDP, 8u, loc);
                        ldp.addr_mode = MADDR_OFFSET;
                        ldp.op_count  = 4u;
                        ldp.ops[0] = mop_preg_def(r0);
                        ldp.ops[1] = mop_preg_def(r1);
                        ldp.ops[2] = mop_preg_use(A64_FP_REG);
                        ldp.ops[3] = mop_imm(cs_gp_off[i]);
                        PIR_VEC_PUSH(&out, ldp);
                    } else {
                        emit_mem_at_base_off(&out, true, r0, A64_FP_REG, cs_gp_off[i],
                                             8u, false, A64_SCRATCH_GP1, loc);
                    }
                }
                if (mf->local_frame_size > 0u || mf->has_alloca_dyn) {
                    MInst rst_sp = minst_make(MI_A64_MOV_SP, 8u, loc);
                    rst_sp.aux_id   = 1u;
                    rst_sp.op_count = 1u;
                    rst_sp.ops[0]   = mop_preg_use(A64_FP_REG);
                    PIR_VEC_PUSH(&out, rst_sp);
                }
                MInst ldp_fp = minst_make(MI_A64_LDP, 8u, loc);
                ldp_fp.addr_mode = MADDR_POST_INDEX;
                ldp_fp.op_count  = 4u;
                ldp_fp.ops[0] = mop_preg_def(A64_FP_REG);
                ldp_fp.ops[1] = mop_preg_def(A64_LR_REG);
                ldp_fp.ops[2] = mop_preg_defuse(A64_SP);
                ldp_fp.ops[3] = mop_imm(16);
                PIR_VEC_PUSH(&out, ldp_fp);

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
