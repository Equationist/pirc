/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lower/lower.h"

/* =========================================================================
 * ARM64 UAL Assembly Emitter (Mach-O Primary + Linux ELF Secondary)
 * ========================================================================= */

typedef struct {
    FILE                *out;
    const PirTargetSpec *target;
    uint32_t             local_seq; /* Unique counter for LL/SC loop labels */
    const uint32_t      *blk_redir; /* Empty jump-only trampoline redirection */
} A64AsmCtx;

static uint32_t redir_blk(const A64AsmCtx *ctx, uint32_t bid) {
    return ctx->blk_redir ? ctx->blk_redir[bid] : bid;
}

static bool is_macho(const PirTargetSpec *target) {
    return target->os == PIR_OS_MACOS;
}

static const char *sym_pfx(const PirTargetSpec *target) {
    return is_macho(target) ? "_" : "";
}

static const char *lbl_pfx(const PirTargetSpec *target) {
    return is_macho(target) ? "L" : ".L";
}

static uint32_t log2_align(uint32_t align) {
    uint32_t p = 0u;
    while (align > 1u) { align >>= 1u; p++; }
    return p;
}

static MCond invert_cond(MCond c) {
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

static void emit_section_directive(FILE *out, const PirTargetSpec *target,
                                   const char *custom_sec, PirGlobalSeg seg,
                                   bool has_reloc, bool is_zero) {
    if (custom_sec && custom_sec[0] != '\0') {
        if (!is_macho(target)) {
            fprintf(out, "\t.section %s\n", custom_sec);
            return;
        }
        if (strchr(custom_sec, ',') != NULL) {
            fprintf(out, "\t.section %s\n", custom_sec);
            return;
        }
        /* Map ELF-style custom section names to valid Mach-O SEGMENT,section syntax. */
        if (strncmp(custom_sec, ".rodata", 7u) == 0) {
            fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
        } else if (strncmp(custom_sec, ".text", 5u) == 0) {
            fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
        } else if (strncmp(custom_sec, ".bss", 4u) == 0) {
            fprintf(out, "\t.section __DATA,__bss\n");
        } else {
            fprintf(out, "\t.section __DATA,__data\n");
        }
        return;
    }
    if (is_macho(target)) {
        switch (seg) {
            case PIR_GSEG_RODATA:
                fprintf(out, "\t.section %s\n", has_reloc ? "__DATA_CONST,__const" : "__TEXT,__const");
                break;
            case PIR_GSEG_DATA:
                fprintf(out, "\t.section __DATA,__data\n");
                break;
            case PIR_GSEG_TLS:
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                break;
        }
    } else {
        switch (seg) {
            case PIR_GSEG_RODATA:
                fprintf(out, "\t.section .rodata\n");
                break;
            case PIR_GSEG_DATA:
                fprintf(out, is_zero ? "\t.bss\n" : "\t.data\n");
                break;
            case PIR_GSEG_TLS:
                fprintf(out, is_zero ? "\t.section .tbss,\"awT\",@nobits\n"
                                     : "\t.section .tdata,\"awT\",@progbits\n");
                break;
        }
    }
}

static bool a64_emit_header(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;

    if (is_macho(target)) {
        fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
    } else {
        fprintf(out, "\t.text\n");
    }
    /* Emit DWARF .file directives (1-based IDs for universal Apple as + GNU as compatibility). */
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        fprintf(out, "\t.file %u \"%s\"\n", mod->files.data[i].id + 1u, mod->files.data[i].path);
    }
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
    const char *sp = sym_pfx(target);
    const char *lp = lbl_pfx(target);
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
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
            if (ins->aux_id == 0u) fprintf(out, "\tmov %s, sp\n", r0);
            else fprintf(out, "\tmov sp, %s\n", r0);
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
            if (is_macho(target)) {
                if (use_got) {
                    fprintf(out, "\tadrp %s, %s%s@GOTPAGE\n", r0, sp, name);
                    fprintf(out, "\tldr %s, [%s, %s%s@GOTPAGEOFF]\n", r0, r0, sp, name);
                } else {
                    fprintf(out, "\tadrp %s, %s%s@PAGE\n", r0, sp, name);
                    fprintf(out, "\tadd %s, %s, %s%s@PAGEOFF\n", r0, r0, sp, name);
                }
            } else {
                if (use_got) {
                    fprintf(out, "\tadrp %s, :got:%s\n", r0, name);
                    fprintf(out, "\tldr %s, [%s, :got_lo12:%s]\n", r0, r0, name);
                } else {
                    fprintf(out, "\tadrp %s, %s\n", r0, name);
                    fprintf(out, "\tadd %s, %s, :lo12:%s\n", r0, r0, name);
                }
            }
            return;
        }

        case MI_A64_TLS_ADDR: {
            const char *name = ins->ops[1].u.sym.name;
            if (is_macho(target)) {
                /* Darwin ARM64 TLV thunk call (returns thread-local address in x0). */
                fprintf(out, "\tadrp x0, %s%s@TLVPPAGE\n", sp, name);
                fprintf(out, "\tldr x0, [x0, %s%s@TLVPPAGEOFF]\n", sp, name);
                fprintf(out, "\tldr x8, [x0]\n");
                fprintf(out, "\tblr x8\n");
            } else {
                /* Linux ELF local-exec TLS */
                fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, 8u);
                fprintf(out, "\tmrs %s, TPIDR_EL0\n", r0);
                fprintf(out, "\tadd %s, %s, :tprel_hi12:%s\n", r0, r0, name);
                fprintf(out, "\tadd %s, %s, :tprel_lo12_nc:%s\n", r0, r0, name);
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

        case MI_A64_BCOND:
            fprintf(out, "\tb.%s %sBB%u_%u\n", cond_str(ins->ops[0].u.cond),
                    lp, mf->fn_index, redir_blk(ctx, ins->ops[1].u.block_id));
            return;

        case MI_A64_CBZ:
        case MI_A64_CBNZ:
            fmt_gp(r0, sizeof(r0), ins->ops[0].u.preg, ins->width);
            fprintf(out, "\t%s %s, %sBB%u_%u\n", ins->op == MI_A64_CBZ ? "cbz" : "cbnz",
                    r0, lp, mf->fn_index, redir_blk(ctx, ins->ops[1].u.block_id));
            return;

        case MI_A64_BL:
            fprintf(out, "\tbl %s%s\n", sp, ins->ops[0].u.sym.name);
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
            } else if (jt->min_val < 0 && -jt->min_val <= 4095) {
                fprintf(out, "\tadd x16, %s, #%" PRId64 "\n", r0, -jt->min_val);
            } else {
                uint64_t mv = (uint64_t)jt->min_val;
                fprintf(out, "\tmovz x17, #%u\n", (unsigned)(mv & 0xFFFFu));
                if ((mv >> 16u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #16\n", (unsigned)((mv >> 16u) & 0xFFFFu));
                if ((mv >> 32u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #32\n", (unsigned)((mv >> 32u) & 0xFFFFu));
                if ((mv >> 48u) & 0xFFFFu) fprintf(out, "\tmovk x17, #%u, lsl #48\n", (unsigned)((mv >> 48u) & 0xFFFFu));
                fprintf(out, "\tsub x16, %s, x17\n", r0);
            }
            fprintf(out, "\tcmp x16, #%u\n", jt->num_entries - 1u);
            fprintf(out, "\tb.hi %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, jt->default_blk));
            if (is_macho(target)) {
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
            fprintf(stderr, "internal error: unexpected MIR op %u in arm64_asm\n", (unsigned)ins->op);
            abort();
    }
}

static bool a64_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const PirFunction *fn = mf->pir_fn;
    const char *sp = sym_pfx(target);
    const char *lp = lbl_pfx(target);

    if (fn->section && fn->section[0] != '\0') {
        emit_section_directive(out, target, fn->section, PIR_GSEG_RODATA, false, false);
    } else if (is_macho(target)) {
        fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
    } else {
        fprintf(out, "\t.text\n");
    }

    if (fn->linkage == PIR_LINK_EXPORT) {
        fprintf(out, "\t.globl %s%s\n", sp, fn->name);
    } else if (fn->linkage == PIR_LINK_WEAK) {
        if (is_macho(target)) {
            fprintf(out, "\t.globl %s%s\n", sp, fn->name);
            fprintf(out, "\t.weak_definition %s%s\n", sp, fn->name);
        } else {
            fprintf(out, "\t.weak %s%s\n", sp, fn->name);
        }
    }
    if (!is_macho(target)) {
        fprintf(out, "\t.type %s, %%function\n", fn->name);
    }
    fprintf(out, "\t.p2align 2\n");
    fprintf(out, "%s%s:\n", sp, fn->name);
    if (mod->files.len > 0u && is_macho(target)) {
        fprintf(out, "%sfunc_begin%u:\n", lp, mf->fn_index);
    }
    fprintf(out, "\t.cfi_startproc\n");

    /* Build empty jump-only trampoline redirection table so coalesced loop back-edges
     * branch directly to their destination with zero empty trampoline blocks. */
    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = nblks ? (uint32_t *)malloc((size_t)nblks * sizeof(uint32_t)) : NULL;
    bool     *blk_skip  = nblks ? (bool *)calloc((size_t)nblks, sizeof(bool)) : NULL;
    if (nblks && (!blk_redir || !blk_skip)) abort();
    for (uint32_t b = 0u; b < nblks; ++b) blk_redir[b] = b;
    for (uint32_t b = 1u; b < nblks; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        if (blk->insts.len == 1u && blk->insts.data[0].op == MI_A64_B) {
            uint32_t dst = blk->insts.data[0].ops[0].u.block_id;
            if (dst < nblks && dst != b) blk_redir[b] = dst;
        }
    }
    for (uint32_t b = 1u; b < nblks; ++b) {
        uint32_t cur = blk_redir[b];
        for (uint32_t hop = 0u; hop < nblks && blk_redir[cur] != cur; ++hop) {
            cur = blk_redir[cur];
        }
        blk_redir[b] = cur;
        if (cur != b) blk_skip[b] = true;
    }
    ctx->blk_redir = blk_redir;

    uint32_t last_loc = 0u;
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
                        fprintf(out, "\tb.%s %sBB%u_%u\n",
                                cond_str(invert_cond(ins->ops[0].u.cond)),
                                lp, mf->fn_index, fdst);
                    }
                    break;
                }
                if (fdst == b && tdst != b) {
                    fprintf(out, "\tb.%s %sBB%u_%u\n",
                            cond_str(invert_cond(ins->ops[0].u.cond)),
                            lp, mf->fn_index, b);
                    fprintf(out, "\tb %sBB%u_%u\n", lp, mf->fn_index, tdst);
                    break;
                }
            }
            /* Avoid redundant jump to immediately following emitted block */
            if (ins->op == MI_A64_B && redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            if (ins->loc_idx != 0u && ins->loc_idx != last_loc &&
                ins->loc_idx < fn->locs.len) {
                const PirSourceLoc *sl = &fn->locs.data[ins->loc_idx];
                fprintf(out, "\t.loc %u %u %u\n", sl->file_id + 1u, sl->line, sl->col);
                last_loc = ins->loc_idx;
            }
            emit_inst(ctx, mf, ins);
        }
    }

    /* Emit PC-relative switch jump tables right after function in __TEXT (zero absolute relocs!). */
    for (uint32_t j = 0u; j < mf->jtables.len; ++j) {
        const MJTable *jt = &mf->jtables.data[j];
        fprintf(out, "\t.p2align 2\n");
        fprintf(out, "%sJT%u_%u:\n", lp, mf->fn_index, j);
        for (uint32_t k = 0u; k < jt->num_entries; ++k) {
            fprintf(out, "\t.long %sBB%u_%u - %sJT%u_%u\n",
                    lp, mf->fn_index, redir_blk(ctx, jt->targets[k]), lp, mf->fn_index, j);
        }
    }

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);

    if (mod->files.len > 0u && is_macho(target)) {
        fprintf(out, "%sfunc_end%u:\n", lp, mf->fn_index);
    }
    fprintf(out, "\t.cfi_endproc\n");
    if (!is_macho(target)) {
        fprintf(out, "\t.size %s, .-%s\n", fn->name, fn->name);
    }
    fprintf(out, "\n");
    return true;
}

static bool global_has_reloc(const PirGlobal *g) {
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        if (g->items[k].kind == PIR_GITEM_RELOC) return true;
    }
    return false;
}

static uint64_t compute_global_bytes(const PirModule *mod, const PirTargetSpec *target,
                                     const PirGlobal *g) {
    if (g->has_type && !g->is_braced) {
        PirTypeLayout ly = pir_type_layout(mod, target, g->type);
        return ly.size > 0u ? ly.size : 1u;
    }
    uint64_t tot = 0u;
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        const PirGlobalItem *gi = &g->items[k];
        switch (gi->kind) {
            case PIR_GITEM_BYTES: tot += gi->len; break;
            case PIR_GITEM_ZERO:  tot += gi->len > 0u ? gi->len : 1u; break;
            case PIR_GITEM_INT:   tot += 1u; break;
            case PIR_GITEM_FLOAT: tot += 8u; break;
            case PIR_GITEM_RELOC: tot += target->ptr_bytes; break;
        }
    }
    return tot > 0u ? tot : 1u;
}

static bool global_is_all_zero(const PirGlobal *g) {
    if (g->is_zeroinit) return true;
    if (g->item_count == 0u) return true;
    if (g->has_type && !g->is_braced && g->item_count == 1u) {
        const PirGlobalItem *gi = &g->items[0];
        if (gi->kind == PIR_GITEM_INT && gi->int_lo == 0u && gi->int_hi == 0u) return true;
        if (gi->kind == PIR_GITEM_FLOAT && gi->float_bits == 0u) return true;
        if (gi->kind == PIR_GITEM_ZERO) return true;
        return false;
    }
    for (uint32_t k = 0u; k < g->item_count; ++k) {
        if (g->items[k].kind != PIR_GITEM_ZERO) return false;
    }
    return true;
}

static void emit_global_data_items(FILE *out, const PirModule *mod, const PirTargetSpec *target,
                                   const PirGlobal *g) {
    const char *sp = sym_pfx(target);
    if (g->is_zeroinit) {
        uint64_t sz = compute_global_bytes(mod, target, g);
        fprintf(out, "\t.zero %" PRIu64 "\n", sz);
        return;
    }
    if (g->has_type && !g->is_braced && g->item_count == 1u) {
        const PirGlobalItem *gi = &g->items[0];
        PirTypeLayout ly = pir_type_layout(mod, target, g->type);
        if (gi->kind == PIR_GITEM_INT) {
            uint64_t x = gi->int_neg ? (uint64_t)(-(int64_t)gi->int_lo) : gi->int_lo;
            if (ly.size == 1u)      fprintf(out, "\t.byte %" PRIu64 "\n", x & 0xFFu);
            else if (ly.size == 2u) fprintf(out, "\t.short %" PRIu64 "\n", x & 0xFFFFu);
            else if (ly.size == 4u) fprintf(out, "\t.long %" PRIu64 "\n", x & 0xFFFFFFFFu);
            else                    fprintf(out, "\t.quad %" PRIu64 "\n", x);
        } else if (gi->kind == PIR_GITEM_FLOAT) {
            if (ly.size == 4u) fprintf(out, "\t.long %" PRIu64 "\n", gi->float_bits & 0xFFFFFFFFu);
            else               fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
        } else if (gi->kind == PIR_GITEM_RELOC) {
            if (gi->sym_offset != 0)
                fprintf(out, "\t.quad %s%s + %" PRId64 "\n", sp, gi->sym_name, gi->sym_offset);
            else
                fprintf(out, "\t.quad %s%s\n", sp, gi->sym_name);
        } else if (gi->kind == PIR_GITEM_ZERO) {
            fprintf(out, "\t.zero %" PRIu64 "\n", gi->len > 0u ? gi->len : ly.size);
        }
        return;
    }

    for (uint32_t k = 0u; k < g->item_count; ++k) {
        const PirGlobalItem *gi = &g->items[k];
        switch (gi->kind) {
            case PIR_GITEM_BYTES:
                for (uint64_t bi = 0u; bi < gi->len; ++bi) {
                    fprintf(out, "\t.byte %u\n", (unsigned)gi->bytes[bi]);
                }
                break;
            case PIR_GITEM_ZERO:
                fprintf(out, "\t.zero %" PRIu64 "\n", gi->len > 0u ? gi->len : 1u);
                break;
            case PIR_GITEM_INT: {
                uint8_t b = (uint8_t)(gi->int_neg ? (uint64_t)(-(int64_t)gi->int_lo) : gi->int_lo);
                fprintf(out, "\t.byte %u\n", (unsigned)b);
                break;
            }
            case PIR_GITEM_FLOAT:
                fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
                break;
            case PIR_GITEM_RELOC:
                if (gi->sym_offset != 0)
                    fprintf(out, "\t.quad %s%s + %" PRId64 "\n", sp, gi->sym_name, gi->sym_offset);
                else
                    fprintf(out, "\t.quad %s%s\n", sp, gi->sym_name);
                break;
        }
    }
}

static bool a64_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const char *sp = sym_pfx(target);

    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->linkage == PIR_LINK_EXTERN) continue;
        uint32_t al = g->align > 0u ? g->align : 1u;
        uint32_t p2 = log2_align(al);
        bool all_zero = global_is_all_zero(g);
        uint64_t sz = compute_global_bytes(mod, target, g);

        /* Darwin Mach-O TLS: __DATA,__thread_vars descriptor + __thread_data / .tbss backing */
        if (is_macho(target) && g->seg == PIR_GSEG_TLS) {
            if (all_zero) {
                fprintf(out, "\t.tbss %s%s$tlv$init, %" PRIu64 ", %u\n", sp, g->name, sz, p2);
            } else {
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
                fprintf(out, "%s%s$tlv$init:\n", sp, g->name);
                emit_global_data_items(out, mod, target, g);
            }
            fprintf(out, "\t.section __DATA,__thread_vars,thread_local_variables\n");
            if (g->linkage == PIR_LINK_EXPORT) {
                fprintf(out, "\t.globl %s%s\n", sp, g->name);
            } else if (g->linkage == PIR_LINK_WEAK) {
                fprintf(out, "\t.globl %s%s\n", sp, g->name);
                fprintf(out, "\t.weak_definition %s%s\n", sp, g->name);
            }
            fprintf(out, "%s%s:\n", sp, g->name);
            fprintf(out, "\t.quad __tlv_bootstrap\n");
            fprintf(out, "\t.quad 0\n");
            fprintf(out, "\t.quad %s%s$tlv$init\n\n", sp, g->name);
            continue;
        }

        /* Mach-O internal zeroinit mutable data in default section -> .zerofill __DATA,__bss */
        if (is_macho(target) && g->seg == PIR_GSEG_DATA && all_zero &&
            (!g->section || g->section[0] == '\0') && g->linkage == PIR_LINK_INTERNAL) {
            fprintf(out, "\t.zerofill __DATA,__bss,%s%s,%" PRIu64 ",%u\n\n", sp, g->name, sz, p2);
            continue;
        }

        bool has_rel = global_has_reloc(g);
        emit_section_directive(out, target, g->section, g->seg, has_rel, all_zero);

        if (g->linkage == PIR_LINK_EXPORT) {
            fprintf(out, "\t.globl %s%s\n", sp, g->name);
        } else if (g->linkage == PIR_LINK_WEAK) {
            if (is_macho(target)) {
                fprintf(out, "\t.globl %s%s\n", sp, g->name);
                fprintf(out, "\t.weak_definition %s%s\n", sp, g->name);
            } else {
                fprintf(out, "\t.weak %s%s\n", sp, g->name);
            }
        }
        if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
        fprintf(out, "%s%s:\n", sp, g->name);
        emit_global_data_items(out, mod, target, g);
        fprintf(out, "\n");
    }
    return true;
}

static bool a64_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    A64AsmCtx *ctx = (A64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const char *lp = lbl_pfx(target);

    if (mod->files.len > 0u && is_macho(target)) {
        /* Minimal DWARF v4 CU + subprograms so macOS ld64 & lldb bind .loc line tables! */
        fprintf(out, "\t.section __DWARF,__debug_abbrev,regular,debug\n");
        fprintf(out, "%spir_abbrev:\n", lp);
        /* Abbrev 1: DW_TAG_compile_unit (17), DW_CHILDREN_yes (1) */
        fprintf(out, "\t.byte 1, 17, 1\n");
        fprintf(out, "\t.byte 3, 8\n");           /* DW_AT_name, DW_FORM_string */
        fprintf(out, "\t.byte 27, 8\n");          /* DW_AT_comp_dir, DW_FORM_string */
        fprintf(out, "\t.byte 16, 23\n");         /* DW_AT_stmt_list, DW_FORM_sec_offset */
        fprintf(out, "\t.byte 0, 0\n");
        /* Abbrev 2: DW_TAG_subprogram (46), DW_CHILDREN_no (0) */
        fprintf(out, "\t.byte 2, 46, 0\n");
        fprintf(out, "\t.byte 17, 1\n");          /* DW_AT_low_pc, DW_FORM_addr */
        fprintf(out, "\t.byte 18, 6\n");          /* DW_AT_high_pc, DW_FORM_data4 */
        fprintf(out, "\t.byte 3, 8\n");           /* DW_AT_name, DW_FORM_string */
        fprintf(out, "\t.byte 0, 0\n");
        fprintf(out, "\t.byte 0\n");

        fprintf(out, "\t.section __DWARF,__debug_info,regular,debug\n");
        fprintf(out, "\t.long %spir_cu_end - %spir_cu_start\n", lp, lp);
        fprintf(out, "%spir_cu_start:\n", lp);
        fprintf(out, "\t.short 4\n");             /* DWARF version 4 */
        fprintf(out, "\t.long 0\n");              /* debug_abbrev_offset */
        fprintf(out, "\t.byte 8\n");              /* address_size = 8 */
        fprintf(out, "\t.byte 1\n");              /* abbrev 1: DW_TAG_compile_unit */
        fprintf(out, "\t.asciz \"%s\"\n", mod->files.data[0].path);
        fprintf(out, "\t.asciz \".\"\n");
        fprintf(out, "\t.long 0\n");              /* stmt_list = 0 */
        for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
            const PirFunction *f = &mod->funcs.data[fi];
            if (!f->is_defined) continue;
            fprintf(out, "\t.byte 2\n");          /* abbrev 2: DW_TAG_subprogram */
            fprintf(out, "\t.quad %sfunc_begin%u\n", lp, fi);
            fprintf(out, "\t.long %sfunc_end%u - %sfunc_begin%u\n", lp, fi, lp, fi);
            fprintf(out, "\t.asciz \"%s\"\n", f->name);
        }
        fprintf(out, "\t.byte 0\n");              /* end of CU children */
        fprintf(out, "%spir_cu_end:\n", lp);
    }

    if (is_macho(target)) {
        fprintf(out, ".subsections_via_symbols\n");
    } else {
        fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
    }
    return true;
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
    if (target->arch != PIR_ARCH_ARM64 && target->arch != PIR_ARCH_X86_64) {
        pir_diag_error(diag, 0u, 0u,
                       "native assembly emission for target '%s' is not supported",
                       target->triple);
        return false;
    }

    PirCodeEmitter em;
    if (target->arch == PIR_ARCH_X86_64) {
        pir_x64_asm_emitter_init(&em, out, target);
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
            if (ok) ok = pir_regalloc_run(&mf, diag);
            if (ok) ok = pir_x64_frame_lower(&mf, diag);
        } else {
            ok = pir_arm64_isel_func(mod, fn, fi, target, &mf, diag);
            if (ok) ok = pir_regalloc_run(&mf, diag);
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
