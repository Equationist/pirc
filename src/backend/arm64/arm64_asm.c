/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/asm_common.h"

/* =========================================================================
 * ARM64 UAL Assembly Emitter (Mach-O Primary + Linux ELF Secondary)
 * ========================================================================= */

typedef struct {
    FILE                *out;
    const PirTargetSpec *target;
    uint32_t             local_seq; /* Unique counter for LL/SC loop labels */
    const uint32_t      *blk_redir; /* Empty jump-only trampoline redirection */
    bool                 need_branch_relax; /* Relax 19-bit b.cond/cbz/cbnz when fn > 512 KiB */
} A64AsmCtx;

static uint32_t redir_blk(const A64AsmCtx *ctx, uint32_t bid) {
    return ctx->blk_redir ? ctx->blk_redir[bid] : bid;
}

static bool a64_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool a64_needs_sym_quote(const char *s) {
    if (!s || !s[0]) return true;
    if (a64_ci_eq(s, "sp") || a64_ci_eq(s, "wsp") ||
        a64_ci_eq(s, "xzr") || a64_ci_eq(s, "wzr") ||
        a64_ci_eq(s, "lr") || a64_ci_eq(s, "fp") ||
        a64_ci_eq(s, "ip0") || a64_ci_eq(s, "ip1") ||
        a64_ci_eq(s, "zt0") || a64_ci_eq(s, "vg") || a64_ci_eq(s, "ffr") ||
        a64_ci_eq(s, "nzcv") || a64_ci_eq(s, "fpcr") ||
        a64_ci_eq(s, "fpsr") || a64_ci_eq(s, "fpmr")) {
        return true;
    }
    unsigned char c = (unsigned char)s[0];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
    if (c == 'x' || c == 'w' || c == 'v' || c == 'q' || c == 'd' ||
        c == 's' || c == 'h' || c == 'b' || c == 'z' || c == 'p') {
        if (s[1] >= '0' && s[1] <= '9') {
            bool has_dot_suffix = (c == 'v' || c == 'z' || c == 'p');
            uint32_t v = (uint32_t)(s[1] - '0');
            if (s[2] == '\0' || (has_dot_suffix && s[2] == '.')) return true;
            if (s[2] >= '0' && s[2] <= '9' &&
                (s[3] == '\0' || (has_dot_suffix && s[3] == '.'))) {
                v = v * 10u + (uint32_t)(s[2] - '0');
                uint32_t lim = (c == 'p') ? 15u : 31u;
                return v <= lim && s[1] != '0';
            }
        }
    }
    return false;
}

static void a64_emit_sym(FILE *out, const PirTargetSpec *target, const char *name) {
    pir_asm_emit_sym(out, target, name, NULL, a64_needs_sym_quote);
}

static const char *cond_str(MCond c) {
    switch (c) {
        case MCOND_EQ: return "eq";
        case MCOND_NE: return "ne";
        case MCOND_CS: return "cs";
        case MCOND_CC: return "cc";
        case MCOND_MI: return "mi";
        case MCOND_PL: return "pl";
        case MCOND_VS: return "vs";
        case MCOND_VC: return "vc";
        case MCOND_HI: return "hi";
        case MCOND_LS: return "ls";
        case MCOND_GE: return "ge";
        case MCOND_LT: return "lt";
        case MCOND_GT: return "gt";
        case MCOND_LE: return "le";
        case MCOND_P:  return "vs";
        case MCOND_NP: return "vc";
    }
    return "eq";
}

static const char *arr_str(MVecArr a) {
    switch (a) {
        case MARR_8B:   return "8b";
        case MARR_4H:   return "4h";
        case MARR_2S:   return "2s";
        case MARR_16B:  return "16b";
        case MARR_8H:   return "8h";
        case MARR_4S:   return "4s";
        case MARR_2D:   return "2d";
        case MARR_NONE: return "16b";
    }
    return "16b";
}

static const char *arr_elem_str(MVecArr a) {
    switch (a) {
        case MARR_8B:  case MARR_16B: return "b";
        case MARR_4H:  case MARR_8H:  return "h";
        case MARR_2S:  case MARR_4S:  return "s";
        case MARR_2D:                 return "d";
        case MARR_NONE:               return "b";
    }
    return "b";
}

static void fmt_gp(char *buf, size_t cap, MPReg p, uint8_t width) {
    if (p == A64_SP) {
        snprintf(buf, cap, "%s", width <= 4u ? "wsp" : "sp");
        return;
    }
    snprintf(buf, cap, "%c%u", width <= 4u ? 'w' : 'x', a64_reg_num(p));
}

static void fmt_fp(char *buf, size_t cap, MPReg p, uint8_t width) {
    char c = (width == 1u) ? 'b' : (width == 2u) ? 'h' : (width == 4u) ? 's' : (width == 8u) ? 'd' : 'q';
    snprintf(buf, cap, "%c%u", c, a64_reg_num(p));
}

static void fmt_v(char *buf, size_t cap, MPReg p) {
    snprintf(buf, cap, "v%u", a64_reg_num(p));
}

static bool a64_emit_header(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;

    pir_asm_emit_section_directive(out, mod, target, NULL, PIR_GSEG_RODATA, false, false, true);
    /* Emit DWARF .file directives (1-based IDs for universal Apple as + GNU as compatibility). */
    pir_asm_emit_dwarf_files(out, mod);
    return true;
}

static void emit_ldr_str(FILE *out, bool is_load, MPReg reg, MPReg base, int64_t off,
                         uint8_t width, bool is_signed) {
    char rbuf[16], bbuf[16];
    if (a64_is_fp_preg(reg)) fmt_fp(rbuf, sizeof(rbuf), reg, width);
    else fmt_gp(rbuf, sizeof(rbuf), reg, width <= 4u ? 4u : 8u);
    fmt_gp(bbuf, sizeof(bbuf), base, 8u);

    bool use_unscaled = !(off >= 0 && width > 0u && (off % width) == 0 && (off / width) <= 4095);
    const char *mnem = NULL;
    if (is_load) {
        if (a64_is_fp_preg(reg)) {
            mnem = use_unscaled ? "ldur" : "ldr";
        } else if (width == 1u) {
            mnem = is_signed ? (use_unscaled ? "ldursb" : "ldrsb") : (use_unscaled ? "ldurb" : "ldrb");
        } else if (width == 2u) {
            mnem = is_signed ? (use_unscaled ? "ldursh" : "ldrsh") : (use_unscaled ? "ldurh" : "ldrh");
        } else {
            mnem = use_unscaled ? "ldur" : "ldr";
        }
    } else {
        if (a64_is_fp_preg(reg)) {
            mnem = use_unscaled ? "stur" : "str";
        } else if (width == 1u) {
            mnem = use_unscaled ? "sturb" : "strb";
        } else if (width == 2u) {
            mnem = use_unscaled ? "sturh" : "strh";
        } else {
            mnem = use_unscaled ? "stur" : "str";
        }
    }

    if (off == 0) {
        fprintf(out, "\t%s %s, [%s]\n", mnem, rbuf, bbuf);
    } else {
        fprintf(out, "\t%s %s, [%s, #%" PRId64 "]\n", mnem, rbuf, bbuf, off);
    }
}

static void emit_inst(A64AsmCtx *ctx, const MFunc *mf, const MInst *ins) {
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);
    char r0[16], r1[16], r2[16], r3[16];

    switch (ins->op) {
        case MI_NOP:
            return;

        case MI_A64_MOVZ:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            if (ins->aux_width == 0u)
                fprintf(out, "\tmovz %s, #%" PRId64 "\n", r0, ins->ops[1].u.imm);
            else
                fprintf(out, "\tmovz %s, #%" PRId64 ", lsl #%u\n", r0, ins->ops[1].u.imm, (unsigned)ins->aux_width);
            return;

        case MI_A64_MOVN:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            if (ins->aux_width == 0u)
                fprintf(out, "\tmovn %s, #%" PRId64 "\n", r0, ins->ops[1].u.imm);
            else
                fprintf(out, "\tmovn %s, #%" PRId64 ", lsl #%u\n", r0, ins->ops[1].u.imm, (unsigned)ins->aux_width);
            return;

        case MI_A64_MOVK:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fprintf(out, "\tmovk %s, #%" PRId64 ", lsl #%u\n", r0, ins->ops[1].u.imm, (unsigned)ins->aux_width);
            return;

        case MI_A64_MOV_RR:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\tmov %s, %s\n", r0, r1);
            return;

        case MI_A64_MOV_SP:
            if (ins->aux_id == 3u) {
                /* Linux/generic static frame stack probe: ops[0] = imm bytes. */
                uint64_t bytes = (uint64_t)ins->ops[0].u.imm;
                fprintf(out, "\tmovz x16, #%u\n", (unsigned)(bytes & 0xFFFFu));
                if ((bytes >> 16u) & 0xFFFFu)
                    fprintf(out, "\tmovk x16, #%u, lsl #16\n", (unsigned)((bytes >> 16u) & 0xFFFFu));
                if ((bytes >> 32u) & 0xFFFFu)
                    fprintf(out, "\tmovk x16, #%u, lsl #32\n", (unsigned)((bytes >> 32u) & 0xFFFFu));
                if ((bytes >> 48u) & 0xFFFFu)
                    fprintf(out, "\tmovk x16, #%u, lsl #48\n", (unsigned)((bytes >> 48u) & 0xFFFFu));
                fprintf(out, "1:\n");
                fprintf(out, "\tcmp x16, #4096\n");
                fprintf(out, "\tb.ls 2f\n");
                fprintf(out, "\tsub sp, sp, #4096\n");
                fprintf(out, "\tldr x17, [sp]\n");
                fprintf(out, "\tsub x16, x16, #4096\n");
                fprintf(out, "\tb 1b\n");
                fprintf(out, "2:\n");
                fprintf(out, "\tsub sp, sp, x16\n");
                fprintf(out, "\tldr x17, [sp]\n");
                return;
            }
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            if (ins->aux_id == 0u) {
                fprintf(out, "\tmov %s, sp\n", r0);
            } else if (ins->aux_id == 2u) {
                /* Dynamic alloca stack probe: step SP down by 4096B to r0, touching each page. */
                fprintf(out, "\tmov x16, sp\n");
                fprintf(out, "\tcmp x16, %s\n", r0);
                fprintf(out, "\tb.ls 3f\n");
                fprintf(out, "1:\n");
                fprintf(out, "\tsub x16, x16, #4096\n");
                fprintf(out, "\tcmp x16, %s\n", r0);
                fprintf(out, "\tb.ls 2f\n");
                fprintf(out, "\tmov sp, x16\n");
                fprintf(out, "\tldr x17, [sp]\n");
                fprintf(out, "\tb 1b\n");
                fprintf(out, "2:\n");
                fprintf(out, "\tmov sp, %s\n", r0);
                fprintf(out, "\tldr x17, [sp]\n");
                fprintf(out, "3:\n");
                fprintf(out, "\tmov sp, %s\n", r0);
            } else {
                fprintf(out, "\tmov sp, %s\n", r0);
            }
            return;

        case MI_A64_ADD_RR: case MI_A64_ADDS_RR:
        case MI_A64_SUB_RR: case MI_A64_SUBS_RR:
        case MI_A64_ADC:    case MI_A64_ADCS:
        case MI_A64_SBC:    case MI_A64_SBCS:
        case MI_A64_MUL:    case MI_A64_SDIV:   case MI_A64_UDIV:
        case MI_A64_AND_RR: case MI_A64_ORR_RR: case MI_A64_EOR_RR:
        case MI_A64_LSL_RR: case MI_A64_LSR_RR: case MI_A64_ASR_RR:
        case MI_A64_ROR_RR: {
            const char *m = (ins->op == MI_A64_ADD_RR)  ? "add"
                          : (ins->op == MI_A64_ADDS_RR) ? "adds"
                          : (ins->op == MI_A64_SUB_RR)  ? "sub"
                          : (ins->op == MI_A64_SUBS_RR) ? "subs"
                          : (ins->op == MI_A64_ADC)     ? "adc"
                          : (ins->op == MI_A64_ADCS)    ? "adcs"
                          : (ins->op == MI_A64_SBC)     ? "sbc"
                          : (ins->op == MI_A64_SBCS)    ? "sbcs"
                          : (ins->op == MI_A64_MUL)     ? "mul"
                          : (ins->op == MI_A64_SDIV)    ? "sdiv"
                          : (ins->op == MI_A64_UDIV)    ? "udiv"
                          : (ins->op == MI_A64_AND_RR)  ? "and"
                          : (ins->op == MI_A64_ORR_RR)  ? "orr"
                          : (ins->op == MI_A64_EOR_RR)  ? "eor"
                          : (ins->op == MI_A64_LSL_RR)  ? "lslv"
                          : (ins->op == MI_A64_LSR_RR)  ? "lsrv"
                          : (ins->op == MI_A64_ASR_RR)  ? "asrv" : "rorv";
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s, %s\n", m, r0, r1, r2);
            return;
        }

        case MI_A64_ADD_RI: case MI_A64_ADDS_RI:
        case MI_A64_SUB_RI: case MI_A64_SUBS_RI:
        case MI_A64_LSL_RI: case MI_A64_LSR_RI: case MI_A64_ASR_RI:
        case MI_A64_ROR_RI: {
            const char *m = (ins->op == MI_A64_ADD_RI)  ? "add"
                          : (ins->op == MI_A64_ADDS_RI) ? "adds"
                          : (ins->op == MI_A64_SUB_RI)  ? "sub"
                          : (ins->op == MI_A64_SUBS_RI) ? "subs"
                          : (ins->op == MI_A64_LSL_RI)  ? "lsl"
                          : (ins->op == MI_A64_LSR_RI)  ? "lsr"
                          : (ins->op == MI_A64_ASR_RI)  ? "asr" : "ror";
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s, #%" PRId64 "\n", m, r0, r1, ins->ops[2].u.imm);
            return;
        }

        case MI_A64_AND_RI: case MI_A64_ORR_RI: case MI_A64_EOR_RI: {
            const char *m = (ins->op == MI_A64_AND_RI) ? "and"
                          : (ins->op == MI_A64_ORR_RI) ? "orr" : "eor";
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s, #0x%" PRIx64 "\n", m, r0, r1, (uint64_t)ins->ops[2].u.imm);
            return;
        }

        case MI_A64_MADD:
        case MI_A64_MSUB:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, ins->width);
            fmt_gp(r3, sizeof(r3), ins->ops[3].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s, %s, %s\n",
                    ins->op == MI_A64_MADD ? "madd" : "msub", r0, r1, r2, r3);
            return;

        case MI_A64_SMULL: case MI_A64_UMULL:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 4u);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, 4u);
            fprintf(out, "\t%s %s, %s, %s\n", ins->op == MI_A64_SMULL ? "smull" : "umull", r0, r1, r2);
            return;

        case MI_A64_SMULH: case MI_A64_UMULH:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, 8u);
            fprintf(out, "\t%s %s, %s, %s\n", ins->op == MI_A64_SMULH ? "smulh" : "umulh", r0, r1, r2);
            return;

        case MI_A64_MVN: case MI_A64_CLZ: case MI_A64_RBIT:
        case MI_A64_REV: case MI_A64_REV16: {
            const char *m = (ins->op == MI_A64_MVN)   ? "mvn"
                          : (ins->op == MI_A64_CLZ)   ? "clz"
                          : (ins->op == MI_A64_RBIT)  ? "rbit"
                          : (ins->op == MI_A64_REV)   ? "rev" : "rev16";
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s\n", m, r0, r1);
            return;
        }

        case MI_A64_SXTB: case MI_A64_SXTH: case MI_A64_SXTW:
        case MI_A64_UXTB: case MI_A64_UXTH: {
            const char *m = (ins->op == MI_A64_SXTB) ? "sxtb"
                          : (ins->op == MI_A64_SXTH) ? "sxth"
                          : (ins->op == MI_A64_SXTW) ? "sxtw"
                          : (ins->op == MI_A64_UXTB) ? "uxtb" : "uxth";
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 4u);
            fprintf(out, "\t%s %s, %s\n", m, r0, r1);
            return;
        }

        case MI_A64_CMP_RR: case MI_A64_TST_RR:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s\n", ins->op == MI_A64_CMP_RR ? "cmp" : "tst", r0, r1);
            return;

        case MI_A64_CMP_RI:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fprintf(out, "\tcmp %s, #%" PRId64 "\n", r0, ins->ops[1].u.imm);
            return;

        case MI_A64_CMP_SXTW:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 4u);
            fprintf(out, "\tcmp %s, %s, sxtw\n", r0, r1);
            return;

        case MI_A64_CMP_ASR63:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u);
            fprintf(out, "\tcmp %s, %s, asr #63\n", r0, r1);
            return;

        case MI_A64_CSET:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fprintf(out, "\tcset %s, %s\n", r0, cond_str(ins->ops[1].u.cond));
            return;

        case MI_A64_CSEL:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, ins->width);
            fprintf(out, "\tcsel %s, %s, %s, %s\n", r0, r1, r2, cond_str(ins->ops[3].u.cond));
            return;

        case MI_A64_LDR:
            emit_ldr_str(out, true, ins->ops[0].u.preg, ins->ops[1].u.preg,
                         ins->ops[2].u.imm, ins->width, ins->is_signed);
            return;

        case MI_A64_STR:
            emit_ldr_str(out, false, ins->ops[0].u.preg, ins->ops[1].u.preg,
                         ins->ops[2].u.imm, ins->width, false);
            return;

        case MI_A64_LDR_REG:
        case MI_A64_STR_REG: {
            bool is_ld = (ins->op == MI_A64_LDR_REG);
            MPReg d = ins->ops[0].u.preg;
            if (a64_is_fp_preg(d)) fmt_fp(r0, sizeof(r0), d, ins->width);
            else fmt_gp(r0, sizeof(r0), d, ins->width <= 4u ? 4u : 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u);
            bool use_sxtw = (ins->aux_lane2 == 1u);
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, use_sxtw ? 4u : 8u);
            const char *m = is_ld
                ? (a64_is_fp_preg(d) ? "ldr" : (ins->width == 1u ? "ldrb" : ins->width == 2u ? "ldrh" : "ldr"))
                : (a64_is_fp_preg(d) ? "str" : (ins->width == 1u ? "strb" : ins->width == 2u ? "strh" : "str"));
            if (use_sxtw) {
                if (ins->aux_width == 0u) {
                    fprintf(out, "\t%s %s, [%s, %s, sxtw]\n", m, r0, r1, r2);
                } else {
                    fprintf(out, "\t%s %s, [%s, %s, sxtw #%u]\n", m, r0, r1, r2, (unsigned)ins->aux_width);
                }
            } else if (ins->aux_width == 0u) {
                fprintf(out, "\t%s %s, [%s, %s]\n", m, r0, r1, r2);
            } else {
                fprintf(out, "\t%s %s, [%s, %s, lsl #%u]\n", m, r0, r1, r2, (unsigned)ins->aux_width);
            }
            return;
        }

        case MI_A64_LDP:
        case MI_A64_STP: {
            const char *m = (ins->op == MI_A64_LDP) ? "ldp" : "stp";
            if (a64_is_fp_preg(ins->ops[0].u.preg)) {
                fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
                fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            } else {
                fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
                fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            }
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, 8u);
            int64_t off = ins->ops[3].u.imm;
            if (ins->addr_mode == MADDR_PRE_INDEX) {
                fprintf(out, "\t%s %s, %s, [%s, #%" PRId64 "]!\n", m, r0, r1, r2, off);
            } else if (ins->addr_mode == MADDR_POST_INDEX) {
                fprintf(out, "\t%s %s, %s, [%s], #%" PRId64 "\n", m, r0, r1, r2, off);
            } else if (off == 0) {
                fprintf(out, "\t%s %s, %s, [%s]\n", m, r0, r1, r2);
            } else {
                fprintf(out, "\t%s %s, %s, [%s, #%" PRId64 "]\n", m, r0, r1, r2, off);
            }
            return;
        }

        case MI_A64_SYM_ADDR: {
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            const char *name = ins->ops[1].u.sym.name;
            bool use_got     = ins->ops[1].u.sym.use_got;
            if (pir_asm_is_macho(target)) {
                if (use_got) {
                    fprintf(out, "\tadrp %s, ", r0); a64_emit_sym(out, target, name); fputs("@GOTPAGE\n", out);
                    fprintf(out, "\tldr %s, [%s, ", r0, r0); a64_emit_sym(out, target, name); fputs("@GOTPAGEOFF]\n", out);
                } else {
                    fprintf(out, "\tadrp %s, ", r0); a64_emit_sym(out, target, name); fputs("@PAGE\n", out);
                    fprintf(out, "\tadd %s, %s, ", r0, r0); a64_emit_sym(out, target, name); fputs("@PAGEOFF\n", out);
                }
            } else {
                if (use_got) {
                    fprintf(out, "\tadrp %s, :got:", r0); a64_emit_sym(out, target, name); fputc('\n', out);
                    fprintf(out, "\tldr %s, [%s, :got_lo12:", r0, r0); a64_emit_sym(out, target, name); fputs("]\n", out);
                } else {
                    fprintf(out, "\tadrp %s, ", r0); a64_emit_sym(out, target, name); fputc('\n', out);
                    fprintf(out, "\tadd %s, %s, :lo12:", r0, r0); a64_emit_sym(out, target, name); fputc('\n', out);
                }
            }
            return;
        }

        case MI_A64_TLS_ADDR: {
            const char *name = ins->ops[1].u.sym.name;
            if (pir_asm_is_macho(target)) {
                /* Darwin ARM64 TLV thunk call (returns thread-local address in x0). */
                fputs("\tadrp x0, ", out); a64_emit_sym(out, target, name); fputs("@TLVPPAGE\n", out);
                fputs("\tldr x0, [x0, ", out); a64_emit_sym(out, target, name); fputs("@TLVPPAGEOFF]\n", out);
                fprintf(out, "\tldr x8, [x0]\n");
                fprintf(out, "\tblr x8\n");
            } else if (ins->ops[1].u.sym.use_got) {
                /* Linux ELF initial-exec TLS for extern/weak symbols */
                fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
                fputs("\tadrp x16, :gottprel:", out); a64_emit_sym(out, target, name); fputc('\n', out);
                fputs("\tldr x16, [x16, :gottprel_lo12:", out); a64_emit_sym(out, target, name); fputs("]\n", out);
                fprintf(out, "\tmrs %s, TPIDR_EL0\n", r0);
                fprintf(out, "\tadd %s, %s, x16\n", r0, r0);
            } else {
                /* Linux ELF local-exec TLS */
                fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
                fprintf(out, "\tmrs %s, TPIDR_EL0\n", r0);
                fprintf(out, "\tadd %s, %s, :tprel_hi12:", r0, r0); a64_emit_sym(out, target, name); fputc('\n', out);
                fprintf(out, "\tadd %s, %s, :tprel_lo12_nc:", r0, r0); a64_emit_sym(out, target, name); fputc('\n', out);
            }
            return;
        }

        case MI_A64_LDAR: {
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width <= 4u ? 4u : 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u);
            const char *m = (ins->width == 1u) ? "ldarb" : (ins->width == 2u) ? "ldarh" : "ldar";
            fprintf(out, "\t%s %s, [%s]\n", m, r0, r1);
            return;
        }

        case MI_A64_STLR: {
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width <= 4u ? 4u : 8u);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u);
            const char *m = (ins->width == 1u) ? "stlrb" : (ins->width == 2u) ? "stlrh" : "stlr";
            fprintf(out, "\t%s %s, [%s]\n", m, r0, r1);
            return;
        }

        case MI_A64_DMB: {
            PirMemOrder ord = (PirMemOrder)ins->aux_id;
            const char *opt = (ord == PIR_ORDER_ACQUIRE) ? "ishld" : "ish";
            fprintf(out, "\tdmb %s\n", opt);
            return;
        }

        case MI_A64_ATOMIC_RMW: {
            uint32_t seq = ctx->local_seq++;
            uint8_t rw = ins->width <= 4u ? 4u : 8u;
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, rw); /* dst_old (early-clobber) */
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 8u); /* ptr */
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, rw); /* val */
            const char *t1 = (rw == 4u) ? "w16" : "x16";
            PirMemOrder ord = (PirMemOrder)ins->aux_id2;
            bool acq = (ord == PIR_ORDER_ACQUIRE || ord == PIR_ORDER_ACQ_REL || ord == PIR_ORDER_SEQ_CST);
            bool rel = (ord == PIR_ORDER_RELEASE || ord == PIR_ORDER_ACQ_REL || ord == PIR_ORDER_SEQ_CST);
            const char *sfx = (ins->width == 1u) ? "b" : (ins->width == 2u) ? "h" : "";
            const char *ldm = acq ? "ldaxr" : "ldxr";
            const char *stm = rel ? "stlxr" : "stxr";
            fprintf(out, "%sARM_%u_%u:\n", lp, mf->fn_index, seq);
            fprintf(out, "\t%s%s %s, [%s]\n", ldm, sfx, r0, r1);
            switch ((PirRmwOp)ins->aux_id) {
                case PIR_RMW_ADD:  fprintf(out, "\tadd %s, %s, %s\n", t1, r0, r2); break;
                case PIR_RMW_SUB:  fprintf(out, "\tsub %s, %s, %s\n", t1, r0, r2); break;
                case PIR_RMW_AND:  fprintf(out, "\tand %s, %s, %s\n", t1, r0, r2); break;
                case PIR_RMW_OR:   fprintf(out, "\torr %s, %s, %s\n", t1, r0, r2); break;
                case PIR_RMW_XOR:  fprintf(out, "\teor %s, %s, %s\n", t1, r0, r2); break;
                case PIR_RMW_XCHG: fprintf(out, "\tmov %s, %s\n", t1, r2); break;
            }
            fprintf(out, "\t%s%s w17, %s, [%s]\n", stm, sfx, t1, r1);
            fprintf(out, "\tcbnz w17, %sARM_%u_%u\n", lp, mf->fn_index, seq);
            return;
        }

        case MI_A64_ATOMIC_CAS: {
            uint32_t seq = ctx->local_seq++;
            uint8_t rw = ins->width <= 4u ? 4u : 8u;
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, rw); /* dst_old */
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, 4u); /* dst_ok */
            fmt_gp(r2, sizeof(r2), ins->ops[2].u.preg, 8u); /* ptr */
            fmt_gp(r3, sizeof(r3), ins->ops[3].u.preg, rw); /* expected */
            char r4[16];
            fmt_gp(r4, sizeof(r4), ins->ops[4].u.preg, rw); /* desired */
            PirMemOrder ord = (PirMemOrder)ins->aux_id;
            bool acq = (ord == PIR_ORDER_ACQUIRE || ord == PIR_ORDER_ACQ_REL || ord == PIR_ORDER_SEQ_CST);
            bool rel = (ord == PIR_ORDER_RELEASE || ord == PIR_ORDER_ACQ_REL || ord == PIR_ORDER_SEQ_CST);
            const char *sfx = (ins->width == 1u) ? "b" : (ins->width == 2u) ? "h" : "";
            const char *ldm = acq ? "ldaxr" : "ldxr";
            const char *stm = rel ? "stlxr" : "stxr";
            fprintf(out, "%sCAS_%u_%u:\n", lp, mf->fn_index, seq);
            fprintf(out, "\t%s%s %s, [%s]\n", ldm, sfx, r0, r2);
            fprintf(out, "\tcmp %s, %s\n", r0, r3);
            fprintf(out, "\tb.ne %sCASF_%u_%u\n", lp, mf->fn_index, seq);
            fprintf(out, "\t%s%s w16, %s, [%s]\n", stm, sfx, r4, r2);
            fprintf(out, "\tcbnz w16, %sCAS_%u_%u\n", lp, mf->fn_index, seq);
            fprintf(out, "\tmov %s, #1\n", r1);
            fprintf(out, "\tb %sCASD_%u_%u\n", lp, mf->fn_index, seq);
            fprintf(out, "%sCASF_%u_%u:\n", lp, mf->fn_index, seq);
            fprintf(out, "\tclrex\n");
            fprintf(out, "\tmov %s, #0\n", r1);
            fprintf(out, "%sCASD_%u_%u:\n", lp, mf->fn_index, seq);
            return;
        }

        case MI_A64_FMOV_RR:
            if (ins->width == 16u) {
                fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
                fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
                fprintf(out, "\tmov %s.16b, %s.16b\n", r0, r1);
            } else {
                fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
                fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
                fprintf(out, "\tfmov %s, %s\n", r0, r1);
            }
            return;

        case MI_A64_FMOV_G2F:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\tfmov %s, %s\n", r0, r1);
            return;

        case MI_A64_FMOV_F2G:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\tfmov %s, %s\n", r0, r1);
            return;

        case MI_A64_FADD: case MI_A64_FSUB: case MI_A64_FMUL: case MI_A64_FDIV: {
            const char *m = (ins->op == MI_A64_FADD) ? "fadd"
                          : (ins->op == MI_A64_FSUB) ? "fsub"
                          : (ins->op == MI_A64_FMUL) ? "fmul" : "fdiv";
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fmt_fp(r2, sizeof(r2), ins->ops[2].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s, %s\n", m, r0, r1, r2);
            return;
        }

        case MI_A64_FNEG:
        case MI_A64_FABS:
        case MI_A64_FSQRT: {
            const char *m = (ins->op == MI_A64_FNEG) ? "fneg"
                          : (ins->op == MI_A64_FABS) ? "fabs" : "fsqrt";
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\t%s %s, %s\n", m, r0, r1);
            return;
        }

        case MI_A64_FCMP:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fprintf(out, "\tfcmp %s, %s\n", r0, r1);
            return;

        case MI_A64_FCSEL:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width);
            fmt_fp(r2, sizeof(r2), ins->ops[2].u.preg, ins->width);
            fprintf(out, "\tfcsel %s, %s, %s, %s\n", r0, r1, r2, cond_str(ins->ops[3].u.cond));
            return;

        case MI_A64_FCVT:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->width == 8u ? 4u : 8u);
            fprintf(out, "\tfcvt %s, %s\n", r0, r1);
            return;

        case MI_A64_SCVTF: case MI_A64_UCVTF:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->aux_width);
            fprintf(out, "\t%s %s, %s\n", ins->op == MI_A64_SCVTF ? "scvtf" : "ucvtf", r0, r1);
            return;

        case MI_A64_FCVTZS: case MI_A64_FCVTZU:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_fp(r1, sizeof(r1), ins->ops[1].u.preg, ins->aux_width);
            fprintf(out, "\t%s %s, %s\n", ins->op == MI_A64_FCVTZS ? "fcvtzs" : "fcvtzu", r0, r1);
            return;

        /* NEON SIMD */
        case MI_A64_VADD:  case MI_A64_VSUB:  case MI_A64_VMUL:
        case MI_A64_VAND:  case MI_A64_VORR:  case MI_A64_VEOR:  case MI_A64_VBSL: case MI_A64_VBIT:
        case MI_A64_VFADD: case MI_A64_VFSUB: case MI_A64_VFMUL: case MI_A64_VFDIV:
        case MI_A64_VFMIN: case MI_A64_VFMAX:
        case MI_A64_VCMEQ: case MI_A64_VCMGT: case MI_A64_VCMGE: case MI_A64_VCMHI: case MI_A64_VCMHS:
        case MI_A64_VFCMEQ: case MI_A64_VFCMGT: case MI_A64_VFCMGE:
        case MI_A64_VSHL_S: case MI_A64_VSHL_U: {
            const char *m = (ins->op == MI_A64_VADD)   ? "add"
                          : (ins->op == MI_A64_VSUB)   ? "sub"
                          : (ins->op == MI_A64_VMUL)   ? "mul"
                          : (ins->op == MI_A64_VAND)   ? "and"
                          : (ins->op == MI_A64_VORR)   ? "orr"
                          : (ins->op == MI_A64_VEOR)   ? "eor"
                          : (ins->op == MI_A64_VBSL)   ? "bsl"
                          : (ins->op == MI_A64_VBIT)   ? "bit"
                          : (ins->op == MI_A64_VFADD)  ? "fadd"
                          : (ins->op == MI_A64_VFSUB)  ? "fsub"
                          : (ins->op == MI_A64_VFMUL)  ? "fmul"
                          : (ins->op == MI_A64_VFDIV)  ? "fdiv"
                          : (ins->op == MI_A64_VFMIN)  ? "fmin"
                          : (ins->op == MI_A64_VFMAX)  ? "fmax"
                          : (ins->op == MI_A64_VCMEQ)  ? "cmeq"
                          : (ins->op == MI_A64_VCMGT)  ? "cmgt"
                          : (ins->op == MI_A64_VCMGE)  ? "cmge"
                          : (ins->op == MI_A64_VCMHI)  ? "cmhi"
                          : (ins->op == MI_A64_VCMHS)  ? "cmhs"
                          : (ins->op == MI_A64_VFCMEQ) ? "fcmeq"
                          : (ins->op == MI_A64_VFCMGT) ? "fcmgt"
                          : (ins->op == MI_A64_VFCMGE) ? "fcmge"
                          : (ins->op == MI_A64_VSHL_S) ? "sshl" : "ushl";
            const char *a = arr_str(ins->arr);
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fmt_v(r2, sizeof(r2), ins->ops[2].u.preg);
            fprintf(out, "\t%s %s.%s, %s.%s, %s.%s\n", m, r0, a, r1, a, r2, a);
            return;
        }

        case MI_A64_VMVN: case MI_A64_VFNEG: case MI_A64_VNEG:
        case MI_A64_VSCVTF: case MI_A64_VUCVTF: case MI_A64_VFCVTZS: case MI_A64_VFCVTZU: {
            const char *m = (ins->op == MI_A64_VMVN)    ? "not"
                          : (ins->op == MI_A64_VFNEG)   ? "fneg"
                          : (ins->op == MI_A64_VNEG)    ? "neg"
                          : (ins->op == MI_A64_VSCVTF)  ? "scvtf"
                          : (ins->op == MI_A64_VUCVTF)  ? "ucvtf"
                          : (ins->op == MI_A64_VFCVTZS) ? "fcvtzs" : "fcvtzu";
            const char *a = arr_str(ins->arr);
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\t%s %s.%s, %s.%s\n", m, r0, a, r1, a);
            return;
        }

        case MI_A64_VDUP_GP:
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->arr == MARR_2D ? 8u : 4u);
            fprintf(out, "\tdup %s.%s, %s\n", r0, arr_str(ins->arr), r1);
            return;

        case MI_A64_VDUP_LANE:
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\tdup %s.%s, %s.%s[0]\n", r0, arr_str(ins->arr), r1, arr_elem_str(ins->arr));
            return;

        case MI_A64_VUMOV:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->arr == MARR_2D ? 8u : 4u);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\tumov %s, %s.%s[%u]\n", r0, r1, arr_elem_str(ins->arr), (unsigned)ins->aux_width);
            return;

        case MI_A64_VFMOV_LANE:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\tmov %s, %s.%s[%u]\n", r0, r1, arr_elem_str(ins->arr), (unsigned)ins->aux_width);
            return;

        case MI_A64_VINS_GP:
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_gp(r1, sizeof(r1), ins->ops[1].u.preg, ins->arr == MARR_2D ? 8u : 4u);
            fprintf(out, "\tins %s.%s[%u], %s\n", r0, arr_elem_str(ins->arr), (unsigned)ins->aux_width, r1);
            return;

        case MI_A64_VINS_LANE:
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\tins %s.%s[%u], %s.%s[%u]\n", r0, arr_elem_str(ins->arr), (unsigned)ins->aux_width,
                    r1, arr_elem_str(ins->arr), (unsigned)ins->aux_lane2);
            return;

        case MI_A64_VCNT:
            fmt_v(r0, sizeof(r0), ins->ops[0].u.preg);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\tcnt %s.8b, %s.8b\n", r0, r1);
            return;

        case MI_A64_VADDV:
            fmt_fp(r0, sizeof(r0), ins->ops[0].u.preg, 1u);
            fmt_v(r1, sizeof(r1), ins->ops[1].u.preg);
            fprintf(out, "\taddv %s, %s.8b\n", r0, r1);
            return;

        /* Control Flow */
        case MI_A64_B:
            fprintf(out, "\tb %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, ins->ops[0].u.block_id));
            return;

        case MI_A64_BCOND: {
            uint32_t dst_b = redir_blk(ctx, ins->ops[1].u.block_id);
            if (ctx->need_branch_relax) {
                fprintf(out, "\tb.%s .+8\n\tb %sBB%u_%u\n",
                        cond_str(pir_asm_invert_cond(ins->ops[0].u.cond)), lp, mf->fn_index, dst_b);
            } else {
                fprintf(out, "\tb.%s %sBB%u_%u\n", cond_str(ins->ops[0].u.cond),
                        lp, mf->fn_index, dst_b);
            }
            return;
        }

        case MI_A64_CBZ:
        case MI_A64_CBNZ: {
            uint32_t dst_b = redir_blk(ctx, ins->ops[1].u.block_id);
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            if (ctx->need_branch_relax) {
                fprintf(out, "\t%s %s, .+8\n\tb %sBB%u_%u\n",
                        ins->op == MI_A64_CBZ ? "cbnz" : "cbz",
                        r0, lp, mf->fn_index, dst_b);
            } else {
                fprintf(out, "\t%s %s, %sBB%u_%u\n", ins->op == MI_A64_CBZ ? "cbz" : "cbnz",
                        r0, lp, mf->fn_index, dst_b);
            }
            return;
        }

        case MI_A64_BL:
            fputs("\tbl ", out);
            a64_emit_sym(out, target, ins->ops[0].u.sym.name);
            fputc('\n', out);
            return;

        case MI_A64_BLR:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            fprintf(out, "\tblr %s\n", r0);
            return;

        case MI_A64_SWITCH_TBL: {
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            uint32_t jtid = ins->ops[1].u.jtable_id;
            const MJTable *jt = &mf->jtables.data[jtid];
            /* x16 = val - min_val */
            if (jt->min_val == 0) {
                fprintf(out, "\tmov x16, %s\n", r0);
            } else if (jt->min_val > 0 && jt->min_val <= 4095) {
                fprintf(out, "\tsub x16, %s, #%" PRId64 "\n", r0, jt->min_val);
            } else if (jt->min_val < 0 && jt->min_val >= -4095) {
                fprintf(out, "\tadd x16, %s, #%" PRId64 "\n", r0, -jt->min_val);
            } else {
                uint64_t mv = (uint64_t)jt->min_val;
                fprintf(out, "\tmovz x17, #%u\n", (unsigned)(mv & 0xFFFFu));
                if ((mv >> 16u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #16\n", (unsigned)((mv >> 16u) & 0xFFFFu));
                if ((mv >> 32u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #32\n", (unsigned)((mv >> 32u) & 0xFFFFu));
                if ((mv >> 48u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #48\n", (unsigned)((mv >> 48u) & 0xFFFFu));
                fprintf(out, "\tsub x16, %s, x17\n", r0);
            }
            uint32_t max_idx = jt->num_entries - 1u;
            if (max_idx <= 4095u) {
                fprintf(out, "\tcmp x16, #%u\n", max_idx);
            } else {
                fprintf(out, "\tmovz x17, #%u\n", max_idx & 0xFFFFu);
                if (max_idx >> 16u) fprintf(out, "\tmovk x17, #%u, lsl #16\n", max_idx >> 16u);
                fprintf(out, "\tcmp x16, x17\n");
            }
            if (ctx->need_branch_relax) {
                fprintf(out, "\tb.ls .+8\n\tb %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, jt->default_blk));
            } else {
                fprintf(out, "\tb.hi %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, jt->default_blk));
            }
            if (pir_asm_is_macho(target)) {
                fprintf(out, "\tadrp x17, %sJT%u_%u@PAGE\n", lp, mf->fn_index, jtid);
                fprintf(out, "\tadd x17, x17, %sJT%u_%u@PAGEOFF\n", lp, mf->fn_index, jtid);
            } else {
                fprintf(out, "\tadrp x17, %sJT%u_%u\n", lp, mf->fn_index, jtid);
                fprintf(out, "\tadd x17, x17, :lo12:%sJT%u_%u\n", lp, mf->fn_index, jtid);
            }
            fprintf(out, "\tldrsw x16, [x17, x16, lsl #2]\n");
            fprintf(out, "\tadd x16, x17, x16\n");
            fprintf(out, "\tbr x16\n");
            return;
        }

        case MI_A64_RET:
            fprintf(out, "\tret\n");
            return;

        case MI_A64_UDF:
            fprintf(out, "\tudf #0\n");
            return;

        /* Pseudo-ops & non-ARM64 opcodes must not reach arm64_asm */
        case MI_COPY: case MI_PCOPY: case MI_FRAME_ADDR:
        case MI_LOAD_SLOT: case MI_STORE_SLOT:
        case MI_LOAD_INARG: case MI_INARG_ADDR:
        case MI_STORE_OUTARG: case MI_COPY_OUTARG_MEM:
        case MI_VA_START: case MI_CALL_ADJ_DOWN: case MI_CALL_ADJ_UP:
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
        case MI_AVR_LI: case MI_AVR_MOV: case MI_AVR_SYM_ADDR:
        case MI_AVR_LD: case MI_AVR_ST: case MI_AVR_PUSH: case MI_AVR_POP:
        case MI_AVR_IN_SP: case MI_AVR_OUT_SP: case MI_AVR_ADJ_SP:
        case MI_AVR_ADD: case MI_AVR_ADDI: case MI_AVR_SUB:
        case MI_AVR_MUL: case MI_AVR_MUL_WIDE:
        case MI_AVR_AND: case MI_AVR_ANDI: case MI_AVR_OR: case MI_AVR_ORI:
        case MI_AVR_XOR: case MI_AVR_XORI:
        case MI_AVR_SHL: case MI_AVR_SHLI: case MI_AVR_SHR: case MI_AVR_SHRI:
        case MI_AVR_SAR: case MI_AVR_SARI: case MI_AVR_SEXT8: case MI_AVR_ZEXT8:
        case MI_AVR_CMP_SET: case MI_AVR_SELECT:
        case MI_AVR_ATOMIC_LD: case MI_AVR_ATOMIC_ST: case MI_AVR_ATOMIC_RMW: case MI_AVR_ATOMIC_CAS:
        case MI_AVR_JMP: case MI_AVR_BR_NZ: case MI_AVR_BR_Z: case MI_AVR_BR_CMP:
        case MI_AVR_CALL: case MI_AVR_ICALL: case MI_AVR_SWITCH_TBL: case MI_AVR_RET: case MI_AVR_TRAP:
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
            fprintf(stderr, "internal error: unexpected MIR op %u in arm64_asm\n", (unsigned)ins->op);
            abort();
    }
}

static bool a64_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const PirFunction *fn = mf->pir_fn;
    const char *lp = pir_asm_lbl_pfx(target);

    pir_asm_emit_func_prologue(out, mod, target, mf, 2u, "%", a64_needs_sym_quote);

    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = NULL;
    bool     *blk_skip  = NULL;
    pir_asm_build_blk_redir(mf, MI_A64_B, &blk_redir, &blk_skip);
    ctx->blk_redir = blk_redir;

    /* Relax 19-bit conditional branches (b.cond/cbz/cbnz, +-1 MiB) when function exceeds ~512 KiB. */
    uint64_t est_insts = 0u;
    for (uint32_t b = 0u; b < nblks; ++b) {
        if (blk_skip[b]) continue;
        const MBlock *blk = &mf->blocks.data[b];
        est_insts += 1u;
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            MOpcode op = blk->insts.data[j].op;
            if (op == MI_A64_SWITCH_TBL)       est_insts += 12u;
            else if (op == MI_A64_ATOMIC_CAS)  est_insts += 10u;
            else if (op == MI_A64_ATOMIC_RMW)  est_insts += 6u;
            else if (op == MI_A64_TLS_ADDR)    est_insts += 4u;
            else if (op == MI_A64_SYM_ADDR)    est_insts += 2u;
            else if (op == MI_A64_MOV_SP &&
                     (blk->insts.data[j].aux_id == 2u || blk->insts.data[j].aux_id == 3u))
                                               est_insts += 14u;
            else                               est_insts += 1u;
        }
    }
    ctx->need_branch_relax = (est_insts * 4u > 524288u);

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
            /* If last two insts are `b.cond T; b F`:
             * - If T == next_live, invert to single `b.!cond F`.
             * - Else if F == b (hot self-loop back-edge!), invert to `b.!cond b; b T`. */
            if (j + 2u == blk->insts.len &&
                ins->op == MI_A64_BCOND &&
                blk->insts.data[j + 1u].op == MI_A64_B) {
                uint32_t tdst = redir_blk(ctx, ins->ops[1].u.block_id);
                uint32_t fdst = redir_blk(ctx, blk->insts.data[j + 1u].ops[0].u.block_id);
                if (tdst == next_live) {
                    if (fdst != next_live) {
                        if (ctx->need_branch_relax) {
                            fprintf(out, "\tb.%s .+8\n\tb %sBB%u_%u\n",
                                    cond_str(ins->ops[0].u.cond), lp, mf->fn_index, fdst);
                        } else {
                            fprintf(out, "\tb.%s %sBB%u_%u\n",
                                    cond_str(pir_asm_invert_cond(ins->ops[0].u.cond)),
                                    lp, mf->fn_index, fdst);
                        }
                    }
                    break;
                }
                if (fdst == b && tdst != b) {
                    if (ctx->need_branch_relax) {
                        fprintf(out, "\tb.%s .+8\n\tb %sBB%u_%u\n",
                                cond_str(ins->ops[0].u.cond), lp, mf->fn_index, b);
                    } else {
                        fprintf(out, "\tb.%s %sBB%u_%u\n",
                                cond_str(pir_asm_invert_cond(ins->ops[0].u.cond)),
                                lp, mf->fn_index, b);
                    }
                    fprintf(out, "\tb %sBB%u_%u\n", lp, mf->fn_index, tdst);
                    break;
                }
            }
            /* Avoid redundant jump to immediately following emitted block */
            if (ins->op == MI_A64_B && redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            pir_asm_emit_dwarf_loc(out, mod, fn, &loc_st, ins->loc_idx);
            emit_inst(ctx, mf, ins);
        }
    }

    /* Emit PC-relative switch jump tables right after function in __TEXT (zero absolute relocs!). */
    pir_asm_emit_jtables(out, target, mf, blk_redir);

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);

    pir_asm_emit_func_epilogue(out, mod, target, mf, a64_needs_sym_quote);
    return true;
}

static bool a64_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    return pir_asm_emit_globals(ctx->out, mod, target, a64_needs_sym_quote);
}

static bool a64_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    return pir_asm_emit_module_footer(ctx->out, mod, target);
}

void pir_arm64_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)calloc(1u, sizeof(A64AsmCtx));
    if (!ctx) abort();
    ctx->out    = out;
    ctx->target = target;

    em->ctx                = ctx;
    em->emit_module_header = a64_emit_header;
    em->emit_function      = a64_emit_function;
    em->emit_globals       = a64_emit_globals;
    em->emit_module_footer = a64_emit_footer;
}

/* =========================================================================
 * Top-Level Backend Driver
 * ========================================================================= */

bool pir_backend_emit_asm(const PirModule *mod, const PirTargetSpec *target,
                          FILE *out, PirDiag *diag) {
    if ((target->arch != PIR_ARCH_ARM64 && target->arch != PIR_ARCH_X86_64 &&
         target->arch != PIR_ARCH_RV32 && target->arch != PIR_ARCH_PPC32 &&
         target->arch != PIR_ARCH_AVR && target->arch != PIR_ARCH_THUMB2) ||
        target->os == PIR_OS_WINDOWS) {
        pir_diag_error(diag, 0u, 0u,
                       "native assembly emission for target '%s' is not supported",
                       target->triple);
        return false;
    }

    PirCodeEmitter em;
    if (target->arch == PIR_ARCH_X86_64) {
        pir_x64_asm_emitter_init(&em, out, target);
    } else if (target->arch == PIR_ARCH_RV32) {
        pir_rv32_asm_emitter_init(&em, out, target);
    } else if (target->arch == PIR_ARCH_PPC32) {
        pir_ppc32_asm_emitter_init(&em, out, target);
    } else if (target->arch == PIR_ARCH_AVR) {
        pir_avr_asm_emitter_init(&em, out, target);
    } else if (target->arch == PIR_ARCH_THUMB2) {
        pir_thumb2_asm_emitter_init(&em, out, target);
    } else {
        pir_arm64_asm_emitter_init(&em, out, target);
    }

    bool ok = em.emit_module_header(&em, mod, target);

    for (uint32_t fi = 0u; fi < mod->funcs.len && ok; ++fi) {
        const PirFunction *fn = &mod->funcs.data[fi];
        if (!fn->is_defined) continue;

        MFunc mf;
        if (target->arch == PIR_ARCH_X86_64) {
            ok = pir_x64_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_x64_frame_lower(&mf, diag);
        } else if (target->arch == PIR_ARCH_RV32) {
            ok = pir_rv32_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_rv32_frame_lower(&mf, diag);
        } else if (target->arch == PIR_ARCH_PPC32) {
            ok = pir_ppc32_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_ppc32_frame_lower(&mf, diag);
        } else if (target->arch == PIR_ARCH_AVR) {
            ok = pir_avr_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_avr_frame_lower(&mf, diag);
        } else if (target->arch == PIR_ARCH_THUMB2) {
            ok = pir_thumb2_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_thumb2_frame_lower(&mf, diag);
        } else {
            ok = pir_arm64_isel_func(mod, fn, fi, target, &mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_pre_ra(&mf, diag);
#endif
            if (ok) ok = pir_regalloc_run(&mf, diag);
#ifdef PIR_VERIFY_ALL
            if (ok) ok = pir_mir_verify_post_ra(&mf, diag);
#endif
            if (ok) ok = pir_arm64_frame_lower(&mf, diag);
        }
        if (ok) ok = em.emit_function(&em, mod, &mf);
        mfunc_destroy(&mf);
    }

    if (ok) ok = em.emit_globals(&em, mod, target);
    if (ok) ok = em.emit_module_footer(&em, mod, target);

    free(em.ctx);
    return ok;
}
