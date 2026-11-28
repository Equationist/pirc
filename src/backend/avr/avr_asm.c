/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * AVR 8-bit / 16-bit-pair (ATmega1280, avr-unknown-elf) ELF Assembly Emitter.
 *
 * Invariants maintained across every emitted instruction:
 *   - r1 (__zero_reg__) == 0 at all instruction boundaries (cleared after every mul).
 *   - Immediate instructions (ldi/subi/sbci/andi/ori/cpi) are only ever emitted on
 *     r16..r31 (pairs 8..15); pairs 1..7 stage cleanly through a free scratch pair.
 *   - Control flow uses 22-bit jmp / call (full 128 KiB Flash range) with local
 *     short-branch inversion so conditional branches never overflow +-64 words.
 *   - Jump tables live in .text (Flash) and dispatch via 16-bit pm() word addresses + elpm/ijmp.
 */

#include "backend/asm_common.h"
#include "backend/backend.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE                *out;
    const PirTargetSpec *target;
    const uint32_t      *blk_redir;
    uint32_t             local_lbl_seq;
} AvrAsmCtx;

static bool avr_needs_sym_quote(const char *name) {
    /* LLVM-MC AVR accepts bare register/SFR identifier names unquoted in labels,
     * call, lo8/hi8/pm_lo8/pm_hi8, and .short pm(...); quoting them breaks call
     * and R_AVR_16_PM relocations. pir_asm_emit_sym still quotes non-bare names. */
    return !name || !name[0];
}

static uint32_t redir_blk(const AvrAsmCtx *ctx, uint32_t b) {
    return ctx->blk_redir ? ctx->blk_redir[b] : b;
}

static MPReg pick_high_tmp(MPReg avoid1, MPReg avoid2, MPReg avoid3) {
    if (AVR_Z != avoid1 && AVR_Z != avoid2 && AVR_Z != avoid3) return AVR_Z;
    if (AVR_X != avoid1 && AVR_X != avoid2 && AVR_X != avoid3) return AVR_X;
    return MREG_NONE;
}

static void emit_li_pair(FILE *out, MPReg dst, uint16_t val) {
    uint32_t lo = avr_lo_reg(dst), hi = avr_hi_reg(dst);
    uint8_t blo = (uint8_t)(val & 0xFFu);
    uint8_t bhi = (uint8_t)((val >> 8u) & 0xFFu);
    if (dst >= AVR_PAIR(8)) {
        if (blo == 0u) fprintf(out, "\tmov r%u, r1\n", lo);
        else           fprintf(out, "\tldi r%u, %u\n", lo, (unsigned)blo);
        if (bhi == 0u) fprintf(out, "\tmov r%u, r1\n", hi);
        else           fprintf(out, "\tldi r%u, %u\n", hi, (unsigned)bhi);
    } else {
        /* dst is r2..r15: stage nonzero bytes through r30 (Z lo, scratch) if needed?
         * Wait: emit_li_pair can be called when dst is any pair; Z is scratch1 (never live
         * across a standalone MI_AVR_LI). */
        if (blo == 0u) {
            fprintf(out, "\tmov r%u, r1\n", lo);
        } else {
            fprintf(out, "\tldi r30, %u\n\tmov r%u, r30\n", (unsigned)blo, lo);
        }
        if (bhi == 0u) {
            fprintf(out, "\tmov r%u, r1\n", hi);
        } else {
            fprintf(out, "\tldi r30, %u\n\tmov r%u, r30\n", (unsigned)bhi, hi);
        }
    }
}

/* Add signed 16-bit immediate to a high pair (r16..r31) in place using subi/sbci. */
static void emit_add_imm_high_inplace(FILE *out, MPReg p, uint16_t imm, uint8_t w) {
    uint32_t lo = avr_lo_reg(p), hi = avr_hi_reg(p);
    uint16_t neg = (uint16_t)(0u - imm);
    if (w == 1u) {
        if ((neg & 0xFFu) != 0u) {
            fprintf(out, "\tsubi r%u, %u\n", lo, (unsigned)(neg & 0xFFu));
        }
        return;
    }
    if (imm == 0u) return;
    fprintf(out, "\tsubi r%u, %u\n", lo, (unsigned)(neg & 0xFFu));
    fprintf(out, "\tsbci r%u, %u\n", hi, (unsigned)((neg >> 8u) & 0xFFu));
}

static void emit_out_sp(FILE *out, MPReg src) {
    uint32_t lo = avr_lo_reg(src), hi = avr_hi_reg(src);
    fprintf(out, "\tin r0, 0x3f\n");
    fprintf(out, "\tcli\n");
    fprintf(out, "\tout 0x3e, r%u\n", hi);
    fprintf(out, "\tout 0x3f, r0\n");
    fprintf(out, "\tout 0x3d, r%u\n", lo);
}

static void emit_cmp_and_cond(FILE *out, MPReg lhs, MPReg rhs, uint8_t w, MCond cc,
                              const char **out_br_true, const char **out_br_false) {
    MPReg a = lhs, b = rhs;
    const char *bt = "breq", *bf = "brne";
    switch (cc) {
        case MCOND_EQ: bt = "breq"; bf = "brne"; break;
        case MCOND_NE: bt = "brne"; bf = "breq"; break;
        case MCOND_CC: bt = "brlo"; bf = "brsh"; break;
        case MCOND_CS: bt = "brsh"; bf = "brlo"; break;
        case MCOND_LT: bt = "brlt"; bf = "brge"; break;
        case MCOND_GE: bt = "brge"; bf = "brlt"; break;
        case MCOND_GT: a = rhs; b = lhs; bt = "brlt"; bf = "brge"; break;
        case MCOND_LE: a = rhs; b = lhs; bt = "brge"; bf = "brlt"; break;
        case MCOND_HI: a = rhs; b = lhs; bt = "brlo"; bf = "brsh"; break;
        case MCOND_LS: a = rhs; b = lhs; bt = "brsh"; bf = "brlo"; break;
        case MCOND_MI: case MCOND_PL: case MCOND_VS: case MCOND_VC:
        case MCOND_P:  case MCOND_NP:
            bt = "breq"; bf = "brne"; break;
    }
    fprintf(out, "\tcp r%u, r%u\n", avr_lo_reg(a), avr_lo_reg(b));
    if (w == 2u) {
        fprintf(out, "\tcpc r%u, r%u\n", avr_hi_reg(a), avr_hi_reg(b));
    }
    *out_br_true  = bt;
    *out_br_false = bf;
}

static bool avr_emit_header(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    AvrAsmCtx *ctx = (AvrAsmCtx *)em->ctx;
    (void)target;
    fprintf(ctx->out, "; Generated by pirc (AVR ATmega1280)\n");
    pir_asm_emit_dwarf_files(ctx->out, mod);
    return true;
}

static void emit_inst(AvrAsmCtx *ctx, const MFunc *mf, const MInst *ins) {
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    switch (ins->op) {
        case MI_NOP:
            return;

        case MI_AVR_LI:
            emit_li_pair(out, ins->ops[0].u.preg, (uint16_t)ins->ops[1].u.imm);
            return;

        case MI_AVR_MOV: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            if (d != s) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
            return;
        }

        case MI_AVR_SYM_ADDR: {
            MPReg d = ins->ops[0].u.preg;
            MPReg t = (d >= AVR_PAIR(8)) ? d : AVR_Z;
            const char *lo_mod = ins->ops[1].u.sym.is_func ? "pm_lo8" : "lo8";
            const char *hi_mod = ins->ops[1].u.sym.is_func ? "pm_hi8" : "hi8";
            fprintf(out, "\tldi r%u, %s(", avr_lo_reg(t), lo_mod);
            pir_asm_emit_sym(out, target, ins->ops[1].u.sym.name, NULL, avr_needs_sym_quote);
            if (!ins->ops[1].u.sym.is_func && ins->ops[1].u.sym.offset != 0) {
                fprintf(out, "%+" PRId64, ins->ops[1].u.sym.offset);
            }
            fputs(")\n", out);
            fprintf(out, "\tldi r%u, %s(", avr_hi_reg(t), hi_mod);
            pir_asm_emit_sym(out, target, ins->ops[1].u.sym.name, NULL, avr_needs_sym_quote);
            if (!ins->ops[1].u.sym.is_func && ins->ops[1].u.sym.offset != 0) {
                fprintf(out, "%+" PRId64, ins->ops[1].u.sym.offset);
            }
            fputs(")\n", out);
            if (t != d) {
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(t));
            }
            return;
        }

        case MI_AVR_LD: {
            MPReg d = ins->ops[0].u.preg;
            MPReg base = ins->ops[1].u.preg;
            int64_t disp = ins->ops[2].u.imm;
            uint8_t w = ins->width;
            if (base == AVR_Y && disp >= 0 && disp + (int64_t)w - 1 <= 63) {
                fprintf(out, "\tldd r%u, Y+%" PRId64 "\n", avr_lo_reg(d), disp);
                if (w == 2u) {
                    fprintf(out, "\tldd r%u, Y+%" PRId64 "\n", avr_hi_reg(d), disp + 1);
                }
                return;
            }
            if (base != AVR_Z) {
                fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(base));
            }
            if (disp != 0) {
                emit_add_imm_high_inplace(out, AVR_Z, (uint16_t)disp, 2u);
            }
            if (w == 1u) {
                fprintf(out, "\tld r%u, Z\n", avr_lo_reg(d));
            } else if (d == AVR_Z) {
                fprintf(out, "\tld r0, Z+\n\tld r31, Z\n\tmov r30, r0\n");
            } else {
                fprintf(out, "\tld r%u, Z+\n\tld r%u, Z\n", avr_lo_reg(d), avr_hi_reg(d));
            }
            return;
        }

        case MI_AVR_ST: {
            MPReg s = ins->ops[0].u.preg;
            MPReg base = ins->ops[1].u.preg;
            int64_t disp = ins->ops[2].u.imm;
            uint8_t w = ins->width;
            if (base == AVR_Y && disp >= 0 && disp + (int64_t)w - 1 <= 63) {
                fprintf(out, "\tstd Y+%" PRId64 ", r%u\n", disp, avr_lo_reg(s));
                if (w == 2u) {
                    fprintf(out, "\tstd Y+%" PRId64 ", r%u\n", disp + 1, avr_hi_reg(s));
                }
                return;
            }
            if (s != AVR_Z) {
                if (base != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(base));
                if (disp != 0) emit_add_imm_high_inplace(out, AVR_Z, (uint16_t)disp, 2u);
                if (w == 1u) fprintf(out, "\tst Z, r%u\n", avr_lo_reg(s));
                else         fprintf(out, "\tst Z+, r%u\n\tst Z, r%u\n", avr_lo_reg(s), avr_hi_reg(s));
            } else {
                /* s == Z (r30:r31): stage Z via stack/r0 so X (cyc_gp / GP0) is never clobbered. */
                if (w == 2u) fprintf(out, "\tpush r31\n");
                fprintf(out, "\tpush r30\n");
                if (base != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(base));
                if (disp != 0) emit_add_imm_high_inplace(out, AVR_Z, (uint16_t)disp, 2u);
                fprintf(out, "\tpop r0\n");
                if (w == 1u) {
                    fprintf(out, "\tst Z, r0\n");
                } else {
                    fprintf(out, "\tst Z+, r0\n\tpop r0\n\tst Z, r0\n");
                }
            }
            return;
        }

        case MI_AVR_PUSH: {
            MPReg p = ins->ops[0].u.preg;
            fprintf(out, "\tpush r%u\n\tpush r%u\n", avr_lo_reg(p), avr_hi_reg(p));
            return;
        }

        case MI_AVR_POP: {
            MPReg p = ins->ops[0].u.preg;
            fprintf(out, "\tpop r%u\n\tpop r%u\n", avr_hi_reg(p), avr_lo_reg(p));
            return;
        }

        case MI_AVR_IN_SP: {
            MPReg d = ins->ops[0].u.preg;
            fprintf(out, "\tin r%u, 0x3d\n\tin r%u, 0x3e\n", avr_lo_reg(d), avr_hi_reg(d));
            return;
        }

        case MI_AVR_OUT_SP:
            emit_out_sp(out, ins->ops[0].u.preg);
            return;

        case MI_AVR_ADJ_SP: {
            uint16_t delta = (uint16_t)ins->ops[0].u.imm;
            if (delta == 0u) return;
            fprintf(out, "\tin r30, 0x3d\n\tin r31, 0x3e\n");
            emit_add_imm_high_inplace(out, AVR_Z, delta, 2u);
            emit_out_sp(out, AVR_Z);
            return;
        }

        case MI_AVR_ADD: {
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, b = ins->ops[2].u.preg;
            MPReg rhs = b;
            if (d == a) {
                rhs = b;
            } else if (d == b) {
                rhs = a;
            } else {
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(a));
                rhs = b;
            }
            fprintf(out, "\tadd r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(rhs));
            if (ins->width == 2u) {
                fprintf(out, "\tadc r%u, r%u\n", avr_hi_reg(d), avr_hi_reg(rhs));
            }
            return;
        }

        case MI_AVR_ADDI: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            uint16_t imm = (uint16_t)ins->ops[2].u.imm;
            if (d >= AVR_PAIR(8)) {
                if (d != s) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
                emit_add_imm_high_inplace(out, d, imm, ins->width);
            } else {
                MPReg t = pick_high_tmp(d, s, MREG_NONE);
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(t), avr_lo_reg(s));
                emit_add_imm_high_inplace(out, t, imm, ins->width);
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(t));
            }
            return;
        }

        case MI_AVR_SUB: {
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, b = ins->ops[2].u.preg;
            if (d == a) {
                fprintf(out, "\tsub r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(b));
                if (ins->width == 2u) fprintf(out, "\tsbc r%u, r%u\n", avr_hi_reg(d), avr_hi_reg(b));
            } else if (d != b) {
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(a));
                fprintf(out, "\tsub r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(b));
                if (ins->width == 2u) fprintf(out, "\tsbc r%u, r%u\n", avr_hi_reg(d), avr_hi_reg(b));
            } else {
                /* d == b && d != a: preserve b using r0 (+ stack for hi byte) */
                if (ins->width == 1u) {
                    fprintf(out, "\tmov r0, r%u\n\tmov r%u, r%u\n\tsub r%u, r0\n",
                            avr_lo_reg(d), avr_lo_reg(d), avr_lo_reg(a), avr_lo_reg(d));
                } else {
                    fprintf(out, "\tpush r%u\n\tmov r0, r%u\n\tmovw r%u, r%u\n"
                                 "\tsub r%u, r0\n\tpop r0\n\tsbc r%u, r0\n",
                            avr_hi_reg(d), avr_lo_reg(d), avr_lo_reg(d), avr_lo_reg(a),
                            avr_lo_reg(d), avr_hi_reg(d));
                }
            }
            return;
        }

        case MI_AVR_MUL: {
            /* dst is early-clobber (dst != a, dst != b). */
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, b = ins->ops[2].u.preg;
            if (ins->width == 1u) {
                fprintf(out, "\tmul r%u, r%u\n\tmov r%u, r0\n\tclr r1\n",
                        avr_lo_reg(a), avr_lo_reg(b), avr_lo_reg(d));
            } else {
                fprintf(out, "\tmul r%u, r%u\n\tmovw r%u, r0\n",
                        avr_lo_reg(a), avr_lo_reg(b), avr_lo_reg(d));
                fprintf(out, "\tmul r%u, r%u\n\tadd r%u, r0\n",
                        avr_hi_reg(a), avr_lo_reg(b), avr_hi_reg(d));
                fprintf(out, "\tmul r%u, r%u\n\tadd r%u, r0\n\tclr r1\n",
                        avr_lo_reg(a), avr_hi_reg(b), avr_hi_reg(d));
            }
            return;
        }

        case MI_AVR_MUL_WIDE: {
            /* dst_lo, dst_hi are early-clobber -> distinct from a, b and each other. */
            MPReg dl = ins->ops[0].u.preg, dh = ins->ops[1].u.preg;
            MPReg a  = ins->ops[2].u.preg, b  = ins->ops[3].u.preg;
            fprintf(out, "\tmul r%u, r%u\n\tmovw r%u, r0\n",
                    avr_lo_reg(a), avr_lo_reg(b), avr_lo_reg(dl));
            fprintf(out, "\tmul r%u, r%u\n\tmovw r%u, r0\n",
                    avr_hi_reg(a), avr_hi_reg(b), avr_lo_reg(dh));
            fprintf(out, "\tmul r%u, r%u\n\tadd r%u, r0\n\tadc r%u, r1\n\tclr r1\n\tadc r%u, r1\n",
                    avr_lo_reg(a), avr_hi_reg(b), avr_hi_reg(dl), avr_lo_reg(dh), avr_hi_reg(dh));
            fprintf(out, "\tmul r%u, r%u\n\tadd r%u, r0\n\tadc r%u, r1\n\tclr r1\n\tadc r%u, r1\n",
                    avr_hi_reg(a), avr_lo_reg(b), avr_hi_reg(dl), avr_lo_reg(dh), avr_hi_reg(dh));
            if (ins->is_signed) {
                uint32_t l1 = ctx->local_lbl_seq++;
                uint32_t l2 = ctx->local_lbl_seq++;
                fprintf(out, "\tsbrs r%u, 7\n\trjmp %smws_%u\n\tsub r%u, r%u\n\tsbc r%u, r%u\n%smws_%u:\n",
                        avr_hi_reg(a), lp, l1,
                        avr_lo_reg(dh), avr_lo_reg(b), avr_hi_reg(dh), avr_hi_reg(b), lp, l1);
                fprintf(out, "\tsbrs r%u, 7\n\trjmp %smws_%u\n\tsub r%u, r%u\n\tsbc r%u, r%u\n%smws_%u:\n",
                        avr_hi_reg(b), lp, l2,
                        avr_lo_reg(dh), avr_lo_reg(a), avr_hi_reg(dh), avr_hi_reg(a), lp, l2);
            }
            return;
        }

        case MI_AVR_AND:
        case MI_AVR_OR:
        case MI_AVR_XOR: {
            const char *mn = (ins->op == MI_AVR_AND) ? "and" : (ins->op == MI_AVR_OR ? "or" : "eor");
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, b = ins->ops[2].u.preg;
            MPReg rhs = b;
            if (d == a) rhs = b;
            else if (d == b) rhs = a;
            else { fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(a)); rhs = b; }
            fprintf(out, "\t%s r%u, r%u\n", mn, avr_lo_reg(d), avr_lo_reg(rhs));
            if (ins->width == 2u) {
                fprintf(out, "\t%s r%u, r%u\n", mn, avr_hi_reg(d), avr_hi_reg(rhs));
            }
            return;
        }

        case MI_AVR_ANDI:
        case MI_AVR_ORI:
        case MI_AVR_XORI: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            uint16_t imm = (uint16_t)ins->ops[2].u.imm;
            uint8_t blo = (uint8_t)(imm & 0xFFu), bhi = (uint8_t)((imm >> 8u) & 0xFFu);
            if (ins->op == MI_AVR_XORI) {
                /* AVR has no eori: load immediate bytes into free high reg + eor. */
                MPReg t = pick_high_tmp(d, s, MREG_NONE);
                bool push_t = (t == MREG_NONE);
                if (d != s) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
                if (push_t) {
                    t = (d == AVR_Z) ? AVR_X : AVR_Z;
                    fprintf(out, "\tpush r%u\n", avr_lo_reg(t));
                }
                if (blo != 0u) fprintf(out, "\tldi r%u, %u\n\teor r%u, r%u\n",
                                       avr_lo_reg(t), (unsigned)blo, avr_lo_reg(d), avr_lo_reg(t));
                if (ins->width == 2u && bhi != 0u) {
                    fprintf(out, "\tldi r%u, %u\n\teor r%u, r%u\n",
                            avr_lo_reg(t), (unsigned)bhi, avr_hi_reg(d), avr_lo_reg(t));
                }
                if (push_t) fprintf(out, "\tpop r%u\n", avr_lo_reg(t));
                return;
            }
            const char *mn = (ins->op == MI_AVR_ANDI) ? "andi" : "ori";
            if (d >= AVR_PAIR(8)) {
                if (d != s) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
                fprintf(out, "\t%s r%u, %u\n", mn, avr_lo_reg(d), (unsigned)blo);
                if (ins->width == 2u) fprintf(out, "\t%s r%u, %u\n", mn, avr_hi_reg(d), (unsigned)bhi);
            } else {
                MPReg t = pick_high_tmp(d, s, MREG_NONE);
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(t), avr_lo_reg(s));
                fprintf(out, "\t%s r%u, %u\n", mn, avr_lo_reg(t), (unsigned)blo);
                if (ins->width == 2u) fprintf(out, "\t%s r%u, %u\n", mn, avr_hi_reg(t), (unsigned)bhi);
                fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(t));
            }
            return;
        }

        case MI_AVR_SHLI:
        case MI_AVR_SHRI:
        case MI_AVR_SARI: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            uint32_t amt = (uint32_t)ins->ops[2].u.imm;
            if (d != s) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
            uint32_t lo = avr_lo_reg(d), hi = avr_hi_reg(d);
            if (ins->width == 1u) {
                const char *mn = (ins->op == MI_AVR_SHLI) ? "lsl"
                               : (ins->op == MI_AVR_SHRI) ? "lsr" : "asr";
                for (uint32_t k = 0u; k < amt; ++k) fprintf(out, "\t%s r%u\n", mn, lo);
            } else {
                if (amt >= 8u) {
                    if (ins->op == MI_AVR_SHLI) {
                        fprintf(out, "\tmov r%u, r%u\n\tmov r%u, r1\n", hi, lo, lo);
                        for (uint32_t k = 8u; k < amt; ++k) fprintf(out, "\tlsl r%u\n", hi);
                    } else if (ins->op == MI_AVR_SHRI) {
                        fprintf(out, "\tmov r%u, r%u\n\tmov r%u, r1\n", lo, hi, hi);
                        for (uint32_t k = 8u; k < amt; ++k) fprintf(out, "\tlsr r%u\n", lo);
                    } else {
                        fprintf(out, "\tmov r%u, r%u\n\tlsl r%u\n\tsbc r%u, r%u\n", lo, hi, hi, hi, hi);
                        for (uint32_t k = 8u; k < amt; ++k) fprintf(out, "\tasr r%u\n", lo);
                    }
                } else {
                    for (uint32_t k = 0u; k < amt; ++k) {
                        if (ins->op == MI_AVR_SHLI)      fprintf(out, "\tlsl r%u\n\trol r%u\n", lo, hi);
                        else if (ins->op == MI_AVR_SHRI) fprintf(out, "\tlsr r%u\n\tror r%u\n", hi, lo);
                        else                             fprintf(out, "\tasr r%u\n\tror r%u\n", hi, lo);
                    }
                }
            }
            return;
        }

        case MI_AVR_SHL:
        case MI_AVR_SHR:
        case MI_AVR_SAR: {
            /* dst is early-clobber. Use r0 as loop counter. */
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, cnt = ins->ops[2].u.preg;
            uint32_t lo = avr_lo_reg(d), hi = avr_hi_reg(d);
            uint32_t l_lp = ctx->local_lbl_seq++;
            uint32_t l_dn = ctx->local_lbl_seq++;
            fprintf(out, "\tmov r0, r%u\n", avr_lo_reg(cnt));
            if (d != a) fprintf(out, "\tmovw r%u, r%u\n", lo, avr_lo_reg(a));
            fprintf(out, "\ttst r0\n\tbreq %ssh_%u\n%ssh_%u:\n", lp, l_dn, lp, l_lp);
            if (ins->width == 1u) {
                const char *mn = (ins->op == MI_AVR_SHL) ? "lsl"
                               : (ins->op == MI_AVR_SHR) ? "lsr" : "asr";
                fprintf(out, "\t%s r%u\n", mn, lo);
            } else if (ins->op == MI_AVR_SHL) {
                fprintf(out, "\tlsl r%u\n\trol r%u\n", lo, hi);
            } else if (ins->op == MI_AVR_SHR) {
                fprintf(out, "\tlsr r%u\n\tror r%u\n", hi, lo);
            } else {
                fprintf(out, "\tasr r%u\n\tror r%u\n", hi, lo);
            }
            fprintf(out, "\tdec r0\n\tbrne %ssh_%u\n%ssh_%u:\n", lp, l_lp, lp, l_dn);
            return;
        }

        case MI_AVR_ZEXT8: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            if (d != s) fprintf(out, "\tmov r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(s));
            fprintf(out, "\tmov r%u, r1\n", avr_hi_reg(d));
            return;
        }

        case MI_AVR_SEXT8: {
            MPReg d = ins->ops[0].u.preg, s = ins->ops[1].u.preg;
            uint32_t lo = avr_lo_reg(d), hi = avr_hi_reg(d);
            if (d != s) fprintf(out, "\tmov r%u, r%u\n", lo, avr_lo_reg(s));
            fprintf(out, "\tmov r%u, r%u\n\tlsl r%u\n\tsbc r%u, r%u\n", hi, lo, hi, hi, hi);
            return;
        }

        case MI_AVR_CMP_SET: {
            MPReg d = ins->ops[0].u.preg, a = ins->ops[1].u.preg, b = ins->ops[2].u.preg;
            const char *bt = NULL, *bf = NULL;
            uint32_t lbl = ctx->local_lbl_seq++;
            emit_cmp_and_cond(out, a, b, ins->width, ins->ops[3].u.cond, &bt, &bf);
            /* mov preserves SREG flags! */
            fprintf(out, "\tmov r%u, r1\n\tmov r%u, r1\n\t%s %scm_%u\n\tinc r%u\n%scm_%u:\n",
                    avr_lo_reg(d), avr_hi_reg(d), bf, lp, lbl, avr_lo_reg(d), lp, lbl);
            return;
        }

        case MI_AVR_SELECT: {
            MPReg d = ins->ops[0].u.preg, cond = ins->ops[1].u.preg;
            MPReg tv = ins->ops[2].u.preg, fv = ins->ops[3].u.preg;
            uint32_t l_t = ctx->local_lbl_seq++;
            uint32_t l_e = ctx->local_lbl_seq++;
            fprintf(out, "\ttst r%u\n\tbrne %ssl_%u\n", avr_lo_reg(cond), lp, l_t);
            if (d != fv) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(fv));
            fprintf(out, "\trjmp %ssl_%u\n%ssl_%u:\n", lp, l_e, lp, l_t);
            if (d != tv) fprintf(out, "\tmovw r%u, r%u\n", avr_lo_reg(d), avr_lo_reg(tv));
            fprintf(out, "%ssl_%u:\n", lp, l_e);
            return;
        }

        case MI_AVR_ATOMIC_LD: {
            MPReg d = ins->ops[0].u.preg, ptr = ins->ops[1].u.preg;
            fprintf(out, "\tin r0, 0x3f\n\tpush r0\n\tcli\n");
            if (ptr != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
            if (ins->width == 1u) {
                fprintf(out, "\tld r%u, Z\n", avr_lo_reg(d));
            } else if (d == AVR_Z) {
                fprintf(out, "\tld r0, Z+\n\tld r31, Z\n\tmov r30, r0\n");
            } else {
                fprintf(out, "\tld r%u, Z+\n\tld r%u, Z\n", avr_lo_reg(d), avr_hi_reg(d));
            }
            fprintf(out, "\tpop r0\n\tout 0x3f, r0\n");
            return;
        }

        case MI_AVR_ATOMIC_ST: {
            MPReg val = ins->ops[0].u.preg, ptr = ins->ops[1].u.preg;
            fprintf(out, "\tin r0, 0x3f\n\tpush r0\n\tcli\n");
            if (val != AVR_Z) {
                if (ptr != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
                if (ins->width == 1u) fprintf(out, "\tst Z, r%u\n", avr_lo_reg(val));
                else                  fprintf(out, "\tst Z+, r%u\n\tst Z, r%u\n", avr_lo_reg(val), avr_hi_reg(val));
            } else {
                if (ptr != AVR_X) fprintf(out, "\tmovw r26, r%u\n", avr_lo_reg(ptr));
                if (ins->width == 1u) fprintf(out, "\tst X, r30\n");
                else                  fprintf(out, "\tst X+, r30\n\tst X, r31\n");
            }
            fprintf(out, "\tpop r0\n\tout 0x3f, r0\n");
            return;
        }

        case MI_AVR_ATOMIC_RMW: {
            /* Pinned in isel: old=r24:r25, ptr=r22:r23, val=r20:r21; X and Z are free! */
            MPReg old_r = ins->ops[0].u.preg, ptr = ins->ops[1].u.preg, val = ins->ops[2].u.preg;
            uint8_t w = ins->width;
            fprintf(out, "\tin r0, 0x3f\n\tpush r0\n\tcli\n");
            fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
            if (w == 1u) {
                fprintf(out, "\tld r%u, Z\n\tmov r26, r%u\n", avr_lo_reg(old_r), avr_lo_reg(old_r));
            } else {
                fprintf(out, "\tld r%u, Z+\n\tld r%u, Z\n\tmovw r26, r%u\n",
                        avr_lo_reg(old_r), avr_hi_reg(old_r), avr_lo_reg(old_r));
            }
            switch ((PirRmwOp)ins->aux_id) {
                case PIR_RMW_ADD:
                    fprintf(out, "\tadd r26, r%u\n", avr_lo_reg(val));
                    if (w == 2u) fprintf(out, "\tadc r27, r%u\n", avr_hi_reg(val));
                    break;
                case PIR_RMW_SUB:
                    fprintf(out, "\tsub r26, r%u\n", avr_lo_reg(val));
                    if (w == 2u) fprintf(out, "\tsbc r27, r%u\n", avr_hi_reg(val));
                    break;
                case PIR_RMW_AND:
                    fprintf(out, "\tand r26, r%u\n", avr_lo_reg(val));
                    if (w == 2u) fprintf(out, "\tand r27, r%u\n", avr_hi_reg(val));
                    break;
                case PIR_RMW_OR:
                    fprintf(out, "\tor r26, r%u\n", avr_lo_reg(val));
                    if (w == 2u) fprintf(out, "\tor r27, r%u\n", avr_hi_reg(val));
                    break;
                case PIR_RMW_XOR:
                    fprintf(out, "\teor r26, r%u\n", avr_lo_reg(val));
                    if (w == 2u) fprintf(out, "\teor r27, r%u\n", avr_hi_reg(val));
                    break;
                case PIR_RMW_XCHG:
                    fprintf(out, "\tmovw r26, r%u\n", avr_lo_reg(val));
                    break;
            }
            fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
            if (w == 1u) fprintf(out, "\tst Z, r26\n");
            else         fprintf(out, "\tst Z+, r26\n\tst Z, r27\n");
            fprintf(out, "\tpop r0\n\tout 0x3f, r0\n");
            return;
        }

        case MI_AVR_ATOMIC_CAS: {
            /* Pinned in isel: old=r24:r25, ok=r22:r23, ptr=r20:r21, exp=r18:r19, des=r14:r15. */
            MPReg old_r = ins->ops[0].u.preg, ok_r = ins->ops[1].u.preg;
            MPReg ptr = ins->ops[2].u.preg, exp_r = ins->ops[3].u.preg, des_r = ins->ops[4].u.preg;
            uint8_t w = ins->width;
            uint32_t l_no = ctx->local_lbl_seq++;
            fprintf(out, "\tin r0, 0x3f\n\tpush r0\n\tcli\n");
            fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
            if (w == 1u) {
                fprintf(out, "\tld r%u, Z\n", avr_lo_reg(old_r));
            } else {
                fprintf(out, "\tld r%u, Z+\n\tld r%u, Z\n", avr_lo_reg(old_r), avr_hi_reg(old_r));
            }
            fprintf(out, "\tmov r%u, r1\n\tmov r%u, r1\n", avr_lo_reg(ok_r), avr_hi_reg(ok_r));
            fprintf(out, "\tcp r%u, r%u\n", avr_lo_reg(old_r), avr_lo_reg(exp_r));
            if (w == 2u) fprintf(out, "\tcpc r%u, r%u\n", avr_hi_reg(old_r), avr_hi_reg(exp_r));
            fprintf(out, "\tbrne %scas_%u\n", lp, l_no);
            fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(ptr));
            if (w == 1u) fprintf(out, "\tst Z, r%u\n", avr_lo_reg(des_r));
            else         fprintf(out, "\tst Z+, r%u\n\tst Z, r%u\n", avr_lo_reg(des_r), avr_hi_reg(des_r));
            fprintf(out, "\tinc r%u\n%scas_%u:\n", avr_lo_reg(ok_r), lp, l_no);
            fprintf(out, "\tpop r0\n\tout 0x3f, r0\n");
            return;
        }

        case MI_AVR_JMP:
            fprintf(out, "\tjmp %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, ins->ops[0].u.block_id));
            return;

        case MI_AVR_BR_NZ:
        case MI_AVR_BR_Z: {
            MPReg r = ins->ops[0].u.preg;
            uint32_t dst_b = redir_blk(ctx, ins->ops[1].u.block_id);
            uint32_t skip = ctx->local_lbl_seq++;
            if (ins->width == 1u) {
                fprintf(out, "\ttst r%u\n", avr_lo_reg(r));
            } else {
                fprintf(out, "\tmov r0, r%u\n\tor r0, r%u\n", avr_lo_reg(r), avr_hi_reg(r));
            }
            fprintf(out, "\t%s %sbr_%u\n\tjmp %sBB%u_%u\n%sbr_%u:\n",
                    (ins->op == MI_AVR_BR_NZ) ? "breq" : "brne",
                    lp, skip, lp, mf->fn_index, dst_b, lp, skip);
            return;
        }

        case MI_AVR_BR_CMP: {
            const char *bt = NULL, *bf = NULL;
            uint32_t dst_b = redir_blk(ctx, ins->ops[3].u.block_id);
            uint32_t skip = ctx->local_lbl_seq++;
            emit_cmp_and_cond(out, ins->ops[0].u.preg, ins->ops[1].u.preg,
                              ins->width, ins->ops[2].u.cond, &bt, &bf);
            fprintf(out, "\t%s %sbc_%u\n\tjmp %sBB%u_%u\n%sbc_%u:\n",
                    bf, lp, skip, lp, mf->fn_index, dst_b, lp, skip);
            return;
        }

        case MI_AVR_CALL:
            fputs("\tcall ", out);
            pir_asm_emit_sym(out, target, ins->ops[0].u.sym.name, NULL, avr_needs_sym_quote);
            fputc('\n', out);
            return;

        case MI_AVR_ICALL: {
            MPReg fn_r = ins->ops[0].u.preg;
            if (fn_r != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(fn_r));
            fputs("\ticall\n", out);
            return;
        }

        case MI_AVR_SWITCH_TBL: {
            MPReg idx = ins->ops[0].u.preg;
            uint32_t jtid = ins->ops[1].u.jtable_id;
            uint32_t def_b = redir_blk(ctx, ins->ops[2].u.block_id);
            const MJTable *jt = &mf->jtables.data[jtid];
            uint32_t l_ok = ctx->local_lbl_seq++;

            /* Bounds check: if (uint16_t)idx >= num_entries -> jmp default. */
            if (idx != AVR_Z) fprintf(out, "\tmovw r30, r%u\n", avr_lo_reg(idx));
            fprintf(out, "\tcpi r30, %u\n", (unsigned)(jt->num_entries & 0xFFu));
            fprintf(out, "\tldi r26, %u\n", (unsigned)((jt->num_entries >> 8u) & 0xFFu));
            fprintf(out, "\tcpc r31, r26\n");
            fprintf(out, "\tbrlo %ssw_%u\n", lp, l_ok);
            fprintf(out, "\tjmp %sBB%u_%u\n", lp, mf->fn_index, def_b);
            fprintf(out, "%ssw_%u:\n", lp, l_ok);

            /* Word address of entry = pm(JT) + idx; byte address in RAMPZ:Z = 2 * word_addr. */
            fprintf(out, "\tldi r26, pm_lo8(%sJT%u_%u)\n", lp, mf->fn_index, jtid);
            fprintf(out, "\tldi r27, pm_hi8(%sJT%u_%u)\n", lp, mf->fn_index, jtid);
            fprintf(out, "\tadd r30, r26\n\tadc r31, r27\n");
            fprintf(out, "\tclr r0\n\tlsl r30\n\trol r31\n\trol r0\n");
            fprintf(out, "\tout 0x3b, r0\n");
            fprintf(out, "\telpm r0, Z+\n\telpm r31, Z\n\tmov r30, r0\n\tijmp\n");
            return;
        }

        case MI_AVR_RET:
            fputs("\tret\n", out);
            return;

        case MI_AVR_TRAP:
            fputs("\tcall __pir_avr_trap\n", out);
            return;

        case MI_COPY: case MI_PCOPY: case MI_FRAME_ADDR: case MI_LOAD_SLOT: case MI_STORE_SLOT:
        case MI_LOAD_INARG: case MI_INARG_ADDR: case MI_STORE_OUTARG: case MI_COPY_OUTARG_MEM:
        case MI_VA_START: case MI_CALL_ADJ_DOWN: case MI_CALL_ADJ_UP:
        case MI_A64_MOVZ: case MI_A64_MOVN: case MI_A64_MOVK: case MI_A64_MOV_RR:
        case MI_A64_ADD_RR: case MI_A64_ADD_RI: case MI_A64_ADDS_RR: case MI_A64_ADDS_RI:
        case MI_A64_SUB_RR: case MI_A64_SUB_RI: case MI_A64_SUBS_RR: case MI_A64_SUBS_RI:
        case MI_A64_ADC: case MI_A64_ADCS: case MI_A64_SBC: case MI_A64_SBCS:
        case MI_A64_MUL: case MI_A64_MADD: case MI_A64_MSUB:
        case MI_A64_SMULL: case MI_A64_UMULL: case MI_A64_SMULH: case MI_A64_UMULH:
        case MI_A64_SDIV: case MI_A64_UDIV:
        case MI_A64_AND_RR: case MI_A64_ORR_RR: case MI_A64_EOR_RR:
        case MI_A64_AND_RI: case MI_A64_ORR_RI: case MI_A64_EOR_RI: case MI_A64_MVN:
        case MI_A64_LSL_RR: case MI_A64_LSR_RR: case MI_A64_ASR_RR: case MI_A64_ROR_RR:
        case MI_A64_LSL_RI: case MI_A64_LSR_RI: case MI_A64_ASR_RI: case MI_A64_ROR_RI:
        case MI_A64_CLZ: case MI_A64_RBIT: case MI_A64_REV: case MI_A64_REV16:
        case MI_A64_SXTB: case MI_A64_SXTH: case MI_A64_SXTW: case MI_A64_UXTB: case MI_A64_UXTH:
        case MI_A64_CMP_RR: case MI_A64_CMP_RI: case MI_A64_CMP_SXTW: case MI_A64_CMP_ASR63:
        case MI_A64_TST_RR: case MI_A64_CSET: case MI_A64_CSEL:
        case MI_A64_LDR: case MI_A64_LDR_REG: case MI_A64_STR: case MI_A64_STR_REG:
        case MI_A64_LDP: case MI_A64_STP: case MI_A64_MOV_SP:
        case MI_A64_SYM_ADDR: case MI_A64_TLS_ADDR:
        case MI_A64_LDAR: case MI_A64_STLR: case MI_A64_DMB:
        case MI_A64_ATOMIC_RMW: case MI_A64_ATOMIC_CAS:
        case MI_A64_FMOV_RR: case MI_A64_FMOV_G2F: case MI_A64_FMOV_F2G:
        case MI_A64_FADD: case MI_A64_FSUB: case MI_A64_FMUL: case MI_A64_FDIV:
        case MI_A64_FNEG: case MI_A64_FABS: case MI_A64_FSQRT:
        case MI_A64_FCMP: case MI_A64_FCSEL: case MI_A64_FCVT:
        case MI_A64_SCVTF: case MI_A64_UCVTF: case MI_A64_FCVTZS: case MI_A64_FCVTZU:
        case MI_A64_VADD: case MI_A64_VSUB: case MI_A64_VMUL:
        case MI_A64_VAND: case MI_A64_VORR: case MI_A64_VEOR: case MI_A64_VMVN:
        case MI_A64_VBSL: case MI_A64_VBIT:
        case MI_A64_VFADD: case MI_A64_VFSUB: case MI_A64_VFMUL: case MI_A64_VFDIV:
        case MI_A64_VFMIN: case MI_A64_VFMAX: case MI_A64_VFNEG:
        case MI_A64_VCMEQ: case MI_A64_VCMGT: case MI_A64_VCMGE: case MI_A64_VCMHI: case MI_A64_VCMHS:
        case MI_A64_VFCMEQ: case MI_A64_VFCMGT: case MI_A64_VFCMGE:
        case MI_A64_VDUP_GP: case MI_A64_VDUP_LANE: case MI_A64_VUMOV: case MI_A64_VFMOV_LANE:
        case MI_A64_VINS_GP: case MI_A64_VINS_LANE: case MI_A64_VSHL_S: case MI_A64_VSHL_U:
        case MI_A64_VNEG: case MI_A64_VCNT: case MI_A64_VADDV:
        case MI_A64_VSCVTF: case MI_A64_VUCVTF: case MI_A64_VFCVTZS: case MI_A64_VFCVTZU:
        case MI_A64_B: case MI_A64_BCOND: case MI_A64_CBZ: case MI_A64_CBNZ:
        case MI_A64_BL: case MI_A64_BLR: case MI_A64_SWITCH_TBL: case MI_A64_RET: case MI_A64_UDF:
        case MI_X64_MOV_RR: case MI_X64_MOV_RI: case MI_X64_MOVSX: case MI_X64_MOVZX:
        case MI_X64_MOV_RM: case MI_X64_MOV_RM_IDX: case MI_X64_MOV_MR: case MI_X64_MOV_MR_IDX:
        case MI_X64_LEA: case MI_X64_LEA_IDX: case MI_X64_PUSH: case MI_X64_POP:
        case MI_X64_MOV_RSP: case MI_X64_SYM_ADDR: case MI_X64_TLS_ADDR:
        case MI_X64_ADD_RR: case MI_X64_ADD_RI: case MI_X64_SUB_RR: case MI_X64_SUB_RI:
        case MI_X64_ADC_RR: case MI_X64_SBB_RR: case MI_X64_IMUL_RR: case MI_X64_IMUL_RRI:
        case MI_X64_MUL_1: case MI_X64_IMUL_1: case MI_X64_CQO: case MI_X64_IDIV: case MI_X64_DIV:
        case MI_X64_AND_RR: case MI_X64_AND_RI: case MI_X64_OR_RR: case MI_X64_OR_RI:
        case MI_X64_XOR_RR: case MI_X64_XOR_RI: case MI_X64_NEG: case MI_X64_NOT:
        case MI_X64_SHL_RI: case MI_X64_SHR_RI: case MI_X64_SAR_RI: case MI_X64_ROR_RI:
        case MI_X64_SHL_CL: case MI_X64_SHR_CL: case MI_X64_SAR_CL: case MI_X64_ROR_CL:
        case MI_X64_BSR: case MI_X64_BSF: case MI_X64_BSWAP:
        case MI_X64_CMP_RR: case MI_X64_CMP_RI: case MI_X64_TEST_RR: case MI_X64_TEST_RI:
        case MI_X64_SETCC: case MI_X64_CMOVCC:
        case MI_X64_MFENCE: case MI_X64_ATOMIC_RMW: case MI_X64_ATOMIC_CAS:
        case MI_X64_MOVX_RR: case MI_X64_MOVX_RM: case MI_X64_MOVX_RM_IDX:
        case MI_X64_MOVX_MR: case MI_X64_MOVX_MR_IDX: case MI_X64_MOV_G2X: case MI_X64_MOV_X2G:
        case MI_X64_ADDSS: case MI_X64_ADDSD: case MI_X64_SUBSS: case MI_X64_SUBSD:
        case MI_X64_MULSS: case MI_X64_MULSD: case MI_X64_DIVSS: case MI_X64_DIVSD:
        case MI_X64_SQRTSS: case MI_X64_SQRTSD: case MI_X64_UCOMISS: case MI_X64_UCOMISD:
        case MI_X64_CVTSS2SD: case MI_X64_CVTSD2SS: case MI_X64_CVTSI2SS: case MI_X64_CVTSI2SD:
        case MI_X64_CVTTSS2SI: case MI_X64_CVTTSD2SI:
        case MI_X64_PADDB: case MI_X64_PADDW: case MI_X64_PADDD: case MI_X64_PADDQ:
        case MI_X64_PSUBB: case MI_X64_PSUBW: case MI_X64_PSUBD: case MI_X64_PSUBQ:
        case MI_X64_PMULLW: case MI_X64_PAND: case MI_X64_POR: case MI_X64_PXOR: case MI_X64_PANDN:
        case MI_X64_ADDPS: case MI_X64_ADDPD: case MI_X64_SUBPS: case MI_X64_SUBPD:
        case MI_X64_MULPS: case MI_X64_MULPD: case MI_X64_DIVPS: case MI_X64_DIVPD:
        case MI_X64_MINPS: case MI_X64_MINPD: case MI_X64_MAXPS: case MI_X64_MAXPD:
        case MI_X64_PCMPEQB: case MI_X64_PCMPEQW: case MI_X64_PCMPEQD:
        case MI_X64_PCMPGTB: case MI_X64_PCMPGTW: case MI_X64_PCMPGTD:
        case MI_X64_CMPPS: case MI_X64_CMPPD: case MI_X64_PSHUFD:
        case MI_X64_PUNPCKLBW: case MI_X64_PUNPCKLWD: case MI_X64_PUNPCKLDQ: case MI_X64_PUNPCKLQDQ:
        case MI_X64_CVTDQ2PS: case MI_X64_CVTTPS2DQ:
        case MI_X64_JMP: case MI_X64_JCC: case MI_X64_CALL: case MI_X64_CALL_IND:
        case MI_X64_SWITCH_TBL: case MI_X64_RET: case MI_X64_UD2:
        case MI_RV32_LI: case MI_RV32_MV: case MI_RV32_SYM_ADDR: case MI_RV32_TLS_ADDR:
        case MI_RV32_LD: case MI_RV32_ST: case MI_RV32_FLD: case MI_RV32_FST: case MI_RV32_MOV_SP:
        case MI_RV32_ADD: case MI_RV32_ADDI: case MI_RV32_SUB:
        case MI_RV32_MUL: case MI_RV32_MULH: case MI_RV32_MULHU:
        case MI_RV32_DIV: case MI_RV32_DIVU: case MI_RV32_REM: case MI_RV32_REMU:
        case MI_RV32_AND: case MI_RV32_ANDI: case MI_RV32_OR: case MI_RV32_ORI:
        case MI_RV32_XOR: case MI_RV32_XORI:
        case MI_RV32_SLL: case MI_RV32_SLLI: case MI_RV32_SRL: case MI_RV32_SRLI:
        case MI_RV32_SRA: case MI_RV32_SRAI:
        case MI_RV32_SLT: case MI_RV32_SLTI: case MI_RV32_SLTU: case MI_RV32_SLTIU:
        case MI_RV32_SEQZ: case MI_RV32_SNEZ:
        case MI_RV32_FENCE: case MI_RV32_ATOMIC_RMW: case MI_RV32_ATOMIC_CAS:
        case MI_RV32_FMV_RR: case MI_RV32_FMV_W_X: case MI_RV32_FMV_X_W: case MI_RV32_LOAD_IMM_F64:
        case MI_RV32_FADD: case MI_RV32_FSUB: case MI_RV32_FMUL: case MI_RV32_FDIV:
        case MI_RV32_FNEG: case MI_RV32_FABS: case MI_RV32_FSQRT:
        case MI_RV32_FEQ: case MI_RV32_FLT: case MI_RV32_FLE:
        case MI_RV32_FCVT_S_D: case MI_RV32_FCVT_D_S:
        case MI_RV32_FCVT_F_W: case MI_RV32_FCVT_F_WU:
        case MI_RV32_FCVT_W_F: case MI_RV32_FCVT_WU_F:
        case MI_RV32_J: case MI_RV32_BCC: case MI_RV32_CALL: case MI_RV32_CALL_IND:
        case MI_RV32_SWITCH_TBL: case MI_RV32_RET: case MI_RV32_UNIMP:
        case MI_PPC_LI: case MI_PPC_MR: case MI_PPC_SYM_ADDR: case MI_PPC_TLS_ADDR:
        case MI_PPC_LD: case MI_PPC_ST: case MI_PPC_STWU: case MI_PPC_FLD: case MI_PPC_FST:
        case MI_PPC_MOV_SP: case MI_PPC_MFLR: case MI_PPC_MTLR:
        case MI_PPC_ADD: case MI_PPC_ADDI: case MI_PPC_SUB:
        case MI_PPC_MULLW: case MI_PPC_MULHW: case MI_PPC_MULHWU:
        case MI_PPC_DIVW: case MI_PPC_DIVWU:
        case MI_PPC_AND: case MI_PPC_ANDI: case MI_PPC_OR: case MI_PPC_ORI:
        case MI_PPC_XOR: case MI_PPC_XORI:
        case MI_PPC_SLW: case MI_PPC_SLWI: case MI_PPC_SRW: case MI_PPC_SRWI:
        case MI_PPC_SRAW: case MI_PPC_SRAWI: case MI_PPC_EXTSB: case MI_PPC_EXTSH:
        case MI_PPC_CMPW: case MI_PPC_CMPWI: case MI_PPC_CMPLW: case MI_PPC_CMPLWI:
        case MI_PPC_SETCC:
        case MI_PPC_SYNC: case MI_PPC_ATOMIC_RMW: case MI_PPC_ATOMIC_CAS:
        case MI_PPC_FMR: case MI_PPC_LOAD_IMM_FP:
        case MI_PPC_FADD: case MI_PPC_FSUB: case MI_PPC_FMUL: case MI_PPC_FDIV:
        case MI_PPC_FNEG: case MI_PPC_FABS: case MI_PPC_FSQRT:
        case MI_PPC_FRSP: case MI_PPC_FCMPU: case MI_PPC_FCTIWZ:
        case MI_PPC_B: case MI_PPC_BC: case MI_PPC_CALL: case MI_PPC_CALL_IND:
        case MI_PPC_SWITCH_TBL: case MI_PPC_RET: case MI_PPC_TRAP:
        case MI_T2_LI: case MI_T2_MOV: case MI_T2_SYM_ADDR:
        case MI_T2_LD: case MI_T2_ST: case MI_T2_PUSH: case MI_T2_POP:
        case MI_T2_VPUSH: case MI_T2_VPOP: case MI_T2_ADJ_SP:
        case MI_T2_ADD: case MI_T2_ADDI: case MI_T2_SUB: case MI_T2_MUL:
        case MI_T2_MUL_WIDE: case MI_T2_DIV:
        case MI_T2_AND: case MI_T2_ANDI: case MI_T2_OR: case MI_T2_ORI:
        case MI_T2_XOR: case MI_T2_XORI:
        case MI_T2_SHL: case MI_T2_SHLI: case MI_T2_LSR: case MI_T2_LSRI:
        case MI_T2_ASR: case MI_T2_ASRI: case MI_T2_EXT: case MI_T2_BITOP:
        case MI_T2_CMP_SET: case MI_T2_SELECT:
        case MI_T2_VLD: case MI_T2_VST: case MI_T2_VMOV_F: case MI_T2_LOAD_IMM_FP:
        case MI_T2_VMOV_G2S: case MI_T2_VMOV_S2G: case MI_T2_VMOV_G2D: case MI_T2_VMOV_D2G:
        case MI_T2_PACK_S2: case MI_T2_UNPACK_S:
        case MI_T2_FADD: case MI_T2_FSUB: case MI_T2_FMUL: case MI_T2_FDIV:
        case MI_T2_FNEG: case MI_T2_FABS: case MI_T2_FSQRT:
        case MI_T2_FCVT_F2F: case MI_T2_FCVT_I2F: case MI_T2_FCVT_F2I:
        case MI_T2_FCMP_SET: case MI_T2_FSELECT:
        case MI_T2_DMB: case MI_T2_ATOMIC_LD: case MI_T2_ATOMIC_ST:
        case MI_T2_ATOMIC_RMW: case MI_T2_ATOMIC_CAS:
        case MI_T2_B: case MI_T2_BC: case MI_T2_BR_NZ: case MI_T2_BR_Z:
        case MI_T2_CALL: case MI_T2_CALL_IND: case MI_T2_SWITCH_TBL:
        case MI_T2_RET: case MI_T2_TRAP:
            fprintf(stderr, "pirc: AVR asm emitter encountered non-AVR opcode %u\n", (unsigned)ins->op);
            abort();
    }
}

static bool avr_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    AvrAsmCtx *ctx = (AvrAsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    pir_asm_emit_func_prologue(out, mod, target, mf, 1u, "@", avr_needs_sym_quote);

    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = NULL;
    bool     *blk_skip  = NULL;
    pir_asm_build_blk_redir(mf, MI_AVR_JMP, &blk_redir, &blk_skip);
    ctx->blk_redir = blk_redir;

    PirDwarfLocState loc_st = { 0u, false };
    for (uint32_t b = 0u; b < nblks; ++b) {
        if (blk_skip[b]) continue;
        const MBlock *blk = &mf->blocks.data[b];
        if (b > 0u) {
            fprintf(out, "%sBB%u_%u:\n", lp, mf->fn_index, b);
        }
        uint32_t next_live = nblks;
        for (uint32_t nb = b + 1u; nb < nblks; ++nb) {
            if (!blk_skip[nb]) { next_live = nb; break; }
        }
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            if (ins->op == MI_AVR_JMP && redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            pir_asm_emit_dwarf_loc(out, mod, mf->pir_fn, &loc_st, ins->loc_idx);
            emit_inst(ctx, mf, ins);
        }
    }

    /* Emit Flash jump tables (.short pm(target)) right after function in .text. */
    if (mf->jtables.len > 0u) {
        fputs("\t.p2align 1\n", out);
        for (uint32_t jt = 0u; jt < mf->jtables.len; ++jt) {
            const MJTable *t = &mf->jtables.data[jt];
            fprintf(out, "%sJT%u_%u:\n", lp, mf->fn_index, jt);
            for (uint32_t k = 0u; k < t->num_entries; ++k) {
                fprintf(out, "\t.short pm(%sBB%u_%u)\n", lp, mf->fn_index, redir_blk(ctx, t->targets[k]));
            }
        }
    }

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);

    pir_asm_emit_func_epilogue(out, mod, target, mf, avr_needs_sym_quote);
    return true;
}

static bool avr_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    AvrAsmCtx *ctx = (AvrAsmCtx *)em->ctx;
    return pir_asm_emit_globals(ctx->out, mod, target, avr_needs_sym_quote);
}

static bool avr_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    AvrAsmCtx *ctx = (AvrAsmCtx *)em->ctx;
    return pir_asm_emit_module_footer(ctx->out, mod, target);
}

void pir_avr_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target) {
    AvrAsmCtx *ctx = (AvrAsmCtx *)calloc(1u, sizeof(AvrAsmCtx));
    if (!ctx) abort();
    ctx->out    = out;
    ctx->target = target;

    em->ctx                = ctx;
    em->emit_module_header = avr_emit_header;
    em->emit_function      = avr_emit_function;
    em->emit_globals       = avr_emit_globals;
    em->emit_module_footer = avr_emit_footer;
}
