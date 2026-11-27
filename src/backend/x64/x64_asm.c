/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "backend/backend.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/asm_common.h"

/* =========================================================================
 * x86-64 Intel-Syntax Assembly Emitter (Mach-O & Linux ELF)
 * ========================================================================= */

typedef struct {
    FILE                *out;
    const PirTargetSpec *target;
    uint32_t             local_seq;
    const uint32_t      *blk_redir;
} X64AsmCtx;

static uint32_t redir_blk(const X64AsmCtx *ctx, uint32_t bid) {
    return ctx->blk_redir ? ctx->blk_redir[bid] : bid;
}

static bool x64_ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return false;
    }
    return *a == '\0' && *b == '\0';
}

static bool x64_needs_sym_quote(const char *s) {
    static const char *const RESERVED[] = {
        /* 64/32/16/8-bit GP & IP & segment regs */
        "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip",
        "eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "eip",
        "ax",  "bx",  "cx",  "dx",  "si",  "di",  "bp",  "sp",  "ip",
        "al",  "bl",  "cl",  "dl",  "ah",  "bh",  "ch",  "dh",
        "sil", "dil", "bpl", "spl",
        "cs",  "ds",  "es",  "fs",  "gs",  "ss",  "st",
        "eiz", "riz", "rflags", "eflags", "flags", "mxcsr", "ssp",
        /* Intel-syntax keywords and operators */
        "byte", "word", "dword", "qword", "tbyte", "oword",
        "xmmword", "ymmword", "zmmword", "fword", "xword",
        "ptr", "offset", "short", "near", "far", "flat", "seg",
        "not", "and", "or", "xor", "shl", "shr", "mod"
    };
    if (!s || !s[0]) return true;
    for (size_t i = 0u; i < sizeof(RESERVED) / sizeof(RESERVED[0]); ++i) {
        if (x64_ci_eq(s, RESERVED[i])) return true;
    }
    /* r8..r15 with optional b/w/d suffix */
    char lo[16];
    size_t len = strlen(s);
    if (len >= sizeof(lo)) return false;
    for (size_t i = 0u; i <= len; ++i) {
        unsigned char c = (unsigned char)s[i];
        lo[i] = (char)((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
    }
    if (lo[0] == 'r' && lo[1] >= '8' && lo[1] <= '9') {
        if (len == 2u) return true;
        if (len == 3u && (lo[2] == 'b' || lo[2] == 'w' || lo[2] == 'd')) return true;
    }
    if (lo[0] == 'r' && lo[1] >= '1' && lo[1] <= '3' && lo[2] >= '0' && lo[2] <= '9') {
        uint32_t rv = (uint32_t)(lo[1] - '0') * 10u + (uint32_t)(lo[2] - '0');
        if (rv >= 10u && rv <= 31u) {
            if (len == 3u) return true;
            if (len == 4u && (lo[3] == 'b' || lo[3] == 'w' || lo[3] == 'd')) return true;
        }
    }
    /* xmm0..31, ymm0..31, zmm0..31 */
    if ((lo[0] == 'x' || lo[0] == 'y' || lo[0] == 'z') &&
        lo[1] == 'm' && lo[2] == 'm' && lo[3] >= '0' && lo[3] <= '9') {
        if (len == 4u) return true;
        if (len == 5u && lo[4] >= '0' && lo[4] <= '9' && lo[3] != '0') {
            uint32_t v = (uint32_t)(lo[3] - '0') * 10u + (uint32_t)(lo[4] - '0');
            if (v <= 31u) return true;
        }
    }
    /* tmm0..7, mm0..7, st0..7, fp0..7, k0..7, cr0..15, dr0..15 */
    if (len == 4u && lo[0] == 't' && lo[1] == 'm' && lo[2] == 'm' &&
        lo[3] >= '0' && lo[3] <= '7') {
        return true;
    }
    if (len == 3u &&
        ((lo[0] == 'm' && lo[1] == 'm') ||
         (lo[0] == 's' && lo[1] == 't') ||
         (lo[0] == 'f' && lo[1] == 'p')) &&
        lo[2] >= '0' && lo[2] <= '7') {
        return true;
    }
    if (len == 2u && lo[0] == 'k' && lo[1] >= '0' && lo[1] <= '7') {
        return true;
    }
    if ((lo[0] == 'c' || lo[0] == 'd') && lo[1] == 'r' && lo[2] >= '0' && lo[2] <= '9') {
        if (len == 3u) return true;
        if (len == 4u && lo[2] == '1' && lo[3] >= '0' && lo[3] <= '5') return true;
    }
    return false;
}

static void x64_emit_sym(FILE *out, const PirTargetSpec *target, const char *name) {
    pir_asm_emit_sym(out, target, name, NULL, x64_needs_sym_quote);
}

static const char *x64_cc_str(MCond c) {
    switch (c) {
        case MCOND_EQ: return "e";
        case MCOND_NE: return "ne";
        case MCOND_CS: return "ae";
        case MCOND_CC: return "b";
        case MCOND_MI: return "s";
        case MCOND_PL: return "ns";
        case MCOND_VS: return "o";
        case MCOND_VC: return "no";
        case MCOND_HI: return "a";
        case MCOND_LS: return "be";
        case MCOND_GE: return "ge";
        case MCOND_LT: return "l";
        case MCOND_GT: return "g";
        case MCOND_LE: return "le";
        case MCOND_P:  return "p";
        case MCOND_NP: return "np";
    }
    return "e";
}

static const char *gp_name(MPReg p, uint8_t width) {
    static const char *R64[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"
    };
    static const char *R32[16] = {
        "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
        "r8d", "r9d", "r10d","r11d","r12d","r13d","r14d","r15d"
    };
    static const char *R16[16] = {
        "ax",  "cx",  "dx",  "bx",  "sp",  "bp",  "si",  "di",
        "r8w", "r9w", "r10w","r11w","r12w","r13w","r14w","r15w"
    };
    static const char *R8[16] = {
        "al",  "cl",  "dl",  "bl",  "spl", "bpl", "sil", "dil",
        "r8b", "r9b", "r10b","r11b","r12b","r13b","r14b","r15b"
    };
    uint32_t n = x64_reg_num(p) & 15u;
    if (width == 1u) return R8[n];
    if (width == 2u) return R16[n];
    if (width <= 4u) return R32[n];
    return R64[n];
}

static void fmt_xmm(char *buf, size_t cap, MPReg p) {
    snprintf(buf, cap, "xmm%u", x64_reg_num(p) & 15u);
}

static const char *ptr_qual(uint8_t width) {
    if (width == 1u)  return "byte ptr";
    if (width == 2u)  return "word ptr";
    if (width == 4u)  return "dword ptr";
    if (width == 8u)  return "qword ptr";
    return "xmmword ptr";
}

static void fmt_mem_base_disp(char *buf, size_t cap, MPReg base, int64_t disp) {
    const char *b = gp_name(base, 8u);
    if (disp == 0) {
        snprintf(buf, cap, "[%s]", b);
    } else if (disp > 0) {
        snprintf(buf, cap, "[%s + %" PRId64 "]", b, disp);
    } else {
        snprintf(buf, cap, "[%s - %" PRId64 "]", b, -disp);
    }
}

static void fmt_mem_idx(char *buf, size_t cap, MPReg base, MPReg idx, uint8_t sh, int64_t disp) {
    const char *b = gp_name(base, 8u);
    const char *i = gp_name(idx, 8u);
    unsigned scale = 1u << (sh & 3u);
    if (scale == 1u) {
        if (disp == 0)       snprintf(buf, cap, "[%s + %s]", b, i);
        else if (disp > 0)   snprintf(buf, cap, "[%s + %s + %" PRId64 "]", b, i, disp);
        else                 snprintf(buf, cap, "[%s + %s - %" PRId64 "]", b, i, -disp);
    } else {
        if (disp == 0)       snprintf(buf, cap, "[%s + %s*%u]", b, i, scale);
        else if (disp > 0)   snprintf(buf, cap, "[%s + %s*%u + %" PRId64 "]", b, i, scale, disp);
        else                 snprintf(buf, cap, "[%s + %s*%u - %" PRId64 "]", b, i, scale, -disp);
    }
}

static bool x64_emit_header(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    FILE *out = ctx->out;

    pir_asm_emit_section_directive(out, mod, target, NULL, PIR_GSEG_RODATA, false, false, true);
    fprintf(out, "\t.intel_syntax noprefix\n");
    pir_asm_emit_dwarf_files(out, mod);
    return true;
}

static void emit_gp_load(FILE *out, MPReg dst, const char *mem, uint8_t width, bool is_signed) {
    if (width == 1u) {
        fprintf(out, "\t%s %s, byte ptr %s\n",
                is_signed ? "movsx" : "movzx", gp_name(dst, 4u), mem);
    } else if (width == 2u) {
        fprintf(out, "\t%s %s, word ptr %s\n",
                is_signed ? "movsx" : "movzx", gp_name(dst, 4u), mem);
    } else if (width == 4u) {
        fprintf(out, "\tmov %s, dword ptr %s\n", gp_name(dst, 4u), mem);
    } else {
        fprintf(out, "\tmov %s, qword ptr %s\n", gp_name(dst, 8u), mem);
    }
}

static void emit_xmm_load_store(FILE *out, bool is_load, MPReg reg, const char *mem, uint8_t width) {
    char xb[16];
    fmt_xmm(xb, sizeof(xb), reg);
    const char *mnem = (width == 4u) ? "movss" : (width == 8u) ? "movsd" : "movups";
    const char *qual = ptr_qual(width);
    if (is_load) {
        fprintf(out, "\t%s %s, %s %s\n", mnem, xb, qual, mem);
    } else {
        fprintf(out, "\t%s %s %s, %s\n", mnem, qual, mem, xb);
    }
}

static void emit_inst(X64AsmCtx *ctx, const MFunc *mf, const MInst *ins) {
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);
    char x0[16], x1[16], mem[64];

    switch (ins->op) {
        case MI_NOP:
            return;

        case MI_X64_MOV_RR:
            fprintf(out, "\tmov %s, %s\n",
                    gp_name(ins->ops[0].u.preg, ins->width),
                    gp_name(ins->ops[1].u.preg, ins->width));
            return;

        case MI_X64_MOV_RI: {
            MPReg dst = ins->ops[0].u.preg;
            uint64_t uv = (uint64_t)ins->ops[1].u.imm;
            if (ins->width <= 4u) {
                fprintf(out, "\tmov %s, %" PRIu32 "\n", gp_name(dst, 4u), (uint32_t)uv);
            } else if (uv <= 0xFFFFFFFFULL) {
                /* 32-bit mov zero-extends into 64-bit register cleanly and compactly */
                fprintf(out, "\tmov %s, %" PRIu32 "\n", gp_name(dst, 4u), (uint32_t)uv);
            } else if ((int64_t)uv >= INT32_MIN && (int64_t)uv <= INT32_MAX) {
                fprintf(out, "\tmov %s, %" PRId64 "\n", gp_name(dst, 8u), (int64_t)uv);
            } else {
                fprintf(out, "\tmovabs %s, %" PRId64 "\n", gp_name(dst, 8u), (int64_t)uv);
            }
            return;
        }

        case MI_X64_MOVSX: {
            MPReg dst = ins->ops[0].u.preg;
            MPReg src = ins->ops[1].u.preg;
            if (ins->aux_width == 4u) {
                fprintf(out, "\tmovsxd %s, %s\n", gp_name(dst, 8u), gp_name(src, 4u));
            } else {
                fprintf(out, "\tmovsx %s, %s\n",
                        gp_name(dst, ins->width <= 4u ? 4u : 8u),
                        gp_name(src, ins->aux_width));
            }
            return;
        }

        case MI_X64_MOVZX:
            fprintf(out, "\tmovzx %s, %s\n",
                    gp_name(ins->ops[0].u.preg, 4u),
                    gp_name(ins->ops[1].u.preg, ins->aux_width));
            return;

        case MI_X64_MOV_RM:
            fmt_mem_base_disp(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.imm);
            emit_gp_load(out, ins->ops[0].u.preg, mem, ins->width, ins->is_signed);
            return;

        case MI_X64_MOV_RM_IDX:
            fmt_mem_idx(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.preg,
                        ins->aux_width, ins->ops[3].u.imm);
            emit_gp_load(out, ins->ops[0].u.preg, mem, ins->width, ins->is_signed);
            return;

        case MI_X64_MOV_MR:
            fmt_mem_base_disp(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.imm);
            fprintf(out, "\tmov %s %s, %s\n",
                    ptr_qual(ins->width), mem, gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_MOV_MR_IDX:
            fmt_mem_idx(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.preg,
                        ins->aux_width, ins->ops[3].u.imm);
            fprintf(out, "\tmov %s %s, %s\n",
                    ptr_qual(ins->width), mem, gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_LEA:
            fmt_mem_base_disp(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.imm);
            fprintf(out, "\tlea %s, %s\n", gp_name(ins->ops[0].u.preg, 8u), mem);
            return;

        case MI_X64_LEA_IDX:
            fmt_mem_idx(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.preg,
                        ins->aux_width, ins->ops[3].u.imm);
            fprintf(out, "\tlea %s, %s\n", gp_name(ins->ops[0].u.preg, 8u), mem);
            return;

        case MI_X64_PUSH:
            fprintf(out, "\tpush %s\n", gp_name(ins->ops[0].u.preg, 8u));
            return;

        case MI_X64_POP:
            fprintf(out, "\tpop %s\n", gp_name(ins->ops[0].u.preg, 8u));
            return;

        case MI_X64_MOV_RSP:
            if (ins->aux_id == 3u) {
                /* Linux/generic static frame stack probe: ops[0] = imm bytes. */
                fprintf(out, "\tmov r11, %lld\n", (long long)ins->ops[0].u.imm);
                fprintf(out, "1:\n");
                fprintf(out, "\tcmp r11, 4096\n");
                fprintf(out, "\tjbe 2f\n");
                fprintf(out, "\tsub rsp, 4096\n");
                fprintf(out, "\tmov r10, qword ptr [rsp]\n");
                fprintf(out, "\tsub r11, 4096\n");
                fprintf(out, "\tjmp 1b\n");
                fprintf(out, "2:\n");
                fprintf(out, "\tsub rsp, r11\n");
                fprintf(out, "\tmov r10, qword ptr [rsp]\n");
                return;
            }
            if (ins->aux_id == 0u) {
                fprintf(out, "\tmov %s, rsp\n", gp_name(ins->ops[0].u.preg, 8u));
            } else if (ins->aux_id == 2u) {
                const char *r0 = gp_name(ins->ops[0].u.preg, 8u);
                fprintf(out, "\tmov r11, rsp\n");
                fprintf(out, "\tcmp r11, %s\n", r0);
                fprintf(out, "\tjbe 3f\n");
                fprintf(out, "1:\n");
                fprintf(out, "\tsub r11, 4096\n");
                fprintf(out, "\tcmp r11, %s\n", r0);
                fprintf(out, "\tjbe 2f\n");
                fprintf(out, "\tmov rsp, r11\n");
                fprintf(out, "\tor qword ptr [rsp], 0\n");
                fprintf(out, "\tjmp 1b\n");
                fprintf(out, "2:\n");
                fprintf(out, "\tmov rsp, %s\n", r0);
                fprintf(out, "\tor qword ptr [rsp], 0\n");
                fprintf(out, "3:\n");
                fprintf(out, "\tmov rsp, %s\n", r0);
            } else {
                fprintf(out, "\tmov rsp, %s\n", gp_name(ins->ops[0].u.preg, 8u));
            }
            return;

        case MI_X64_SYM_ADDR: {
            const char *dst = gp_name(ins->ops[0].u.preg, 8u);
            const char *name = ins->ops[1].u.sym.name;
            bool use_got     = ins->ops[1].u.sym.use_got;
            if (use_got) {
                fprintf(out, "\tmov %s, qword ptr [rip + ", dst);
                x64_emit_sym(out, target, name);
                fputs("@GOTPCREL]\n", out);
            } else {
                fprintf(out, "\tlea %s, [rip + ", dst);
                x64_emit_sym(out, target, name);
                fputs("]\n", out);
            }
            return;
        }

        case MI_X64_TLS_ADDR: {
            const char *name = ins->ops[1].u.sym.name;
            if (pir_asm_is_macho(target)) {
                fputs("\tmov rdi, qword ptr [rip + ", out);
                x64_emit_sym(out, target, name);
                fputs("@TLVP]\n", out);
                fprintf(out, "\tcall qword ptr [rdi]\n");
            } else {
                const char *dst = gp_name(ins->ops[0].u.preg, 8u);
                fprintf(out, "\tmov %s, qword ptr fs:[0]\n", dst);
                if (ins->ops[1].u.sym.use_got) {
                    fprintf(out, "\tadd %s, qword ptr [rip + ", dst);
                    x64_emit_sym(out, target, name);
                    fputs("@GOTTPOFF]\n", out);
                } else {
                    fprintf(out, "\tlea %s, [%s + ", dst, dst);
                    x64_emit_sym(out, target, name);
                    fputs("@TPOFF]\n", out);
                }
            }
            return;
        }

        case MI_X64_ADD_RR: case MI_X64_SUB_RR:
        case MI_X64_ADC_RR: case MI_X64_SBB_RR:
        case MI_X64_IMUL_RR:
        case MI_X64_AND_RR: case MI_X64_OR_RR: case MI_X64_XOR_RR:
        case MI_X64_CMP_RR: case MI_X64_TEST_RR: {
            const char *m = (ins->op == MI_X64_ADD_RR)  ? "add"
                          : (ins->op == MI_X64_SUB_RR)  ? "sub"
                          : (ins->op == MI_X64_ADC_RR)  ? "adc"
                          : (ins->op == MI_X64_SBB_RR)  ? "sbb"
                          : (ins->op == MI_X64_IMUL_RR) ? "imul"
                          : (ins->op == MI_X64_AND_RR)  ? "and"
                          : (ins->op == MI_X64_OR_RR)   ? "or"
                          : (ins->op == MI_X64_XOR_RR)  ? "xor"
                          : (ins->op == MI_X64_CMP_RR)  ? "cmp" : "test";
            fprintf(out, "\t%s %s, %s\n", m,
                    gp_name(ins->ops[0].u.preg, ins->width),
                    gp_name(ins->ops[1].u.preg, ins->width));
            return;
        }

        case MI_X64_ADD_RI: case MI_X64_SUB_RI:
        case MI_X64_AND_RI: case MI_X64_OR_RI: case MI_X64_XOR_RI:
        case MI_X64_CMP_RI: case MI_X64_TEST_RI:
        case MI_X64_SHL_RI: case MI_X64_SHR_RI: case MI_X64_SAR_RI: case MI_X64_ROR_RI: {
            const char *m = (ins->op == MI_X64_ADD_RI)  ? "add"
                          : (ins->op == MI_X64_SUB_RI)  ? "sub"
                          : (ins->op == MI_X64_AND_RI)  ? "and"
                          : (ins->op == MI_X64_OR_RI)   ? "or"
                          : (ins->op == MI_X64_XOR_RI)  ? "xor"
                          : (ins->op == MI_X64_CMP_RI)  ? "cmp"
                          : (ins->op == MI_X64_TEST_RI) ? "test"
                          : (ins->op == MI_X64_SHL_RI)  ? "shl"
                          : (ins->op == MI_X64_SHR_RI)  ? "shr"
                          : (ins->op == MI_X64_SAR_RI)  ? "sar" : "ror";
            fprintf(out, "\t%s %s, %" PRId64 "\n", m,
                    gp_name(ins->ops[0].u.preg, ins->width),
                    ins->ops[1].u.imm);
            return;
        }

        case MI_X64_IMUL_RRI:
            fprintf(out, "\timul %s, %s, %" PRId64 "\n",
                    gp_name(ins->ops[0].u.preg, ins->width),
                    gp_name(ins->ops[1].u.preg, ins->width),
                    ins->ops[2].u.imm);
            return;

        case MI_X64_MUL_1:
            fprintf(out, "\tmul %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_IMUL_1:
            fprintf(out, "\timul %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_CQO:
            fprintf(out, "\t%s\n", ins->width <= 4u ? "cdq" : "cqo");
            return;

        case MI_X64_IDIV:
            fprintf(out, "\tidiv %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_DIV:
            fprintf(out, "\tdiv %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_NEG:
            fprintf(out, "\tneg %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_NOT:
            fprintf(out, "\tnot %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            return;

        case MI_X64_SHL_CL: case MI_X64_SHR_CL: case MI_X64_SAR_CL: case MI_X64_ROR_CL: {
            const char *m = (ins->op == MI_X64_SHL_CL) ? "shl"
                          : (ins->op == MI_X64_SHR_CL) ? "shr"
                          : (ins->op == MI_X64_SAR_CL) ? "sar" : "ror";
            fprintf(out, "\t%s %s, cl\n", m, gp_name(ins->ops[0].u.preg, ins->width));
            return;
        }

        case MI_X64_BSR:
            fprintf(out, "\tbsr %s, %s\n",
                    gp_name(ins->ops[0].u.preg, ins->width),
                    gp_name(ins->ops[1].u.preg, ins->width));
            return;

        case MI_X64_BSF:
            fprintf(out, "\tbsf %s, %s\n",
                    gp_name(ins->ops[0].u.preg, ins->width),
                    gp_name(ins->ops[1].u.preg, ins->width));
            return;

        case MI_X64_BSWAP:
            if (ins->width == 2u) {
                fprintf(out, "\trol %s, 8\n", gp_name(ins->ops[0].u.preg, 2u));
                fprintf(out, "\tmovzx %s, %s\n",
                        gp_name(ins->ops[0].u.preg, 4u), gp_name(ins->ops[0].u.preg, 2u));
            } else {
                fprintf(out, "\tbswap %s\n", gp_name(ins->ops[0].u.preg, ins->width));
            }
            return;

        case MI_X64_SETCC: {
            MPReg d = ins->ops[0].u.preg;
            fprintf(out, "\tset%s %s\n", x64_cc_str(ins->ops[1].u.cond), gp_name(d, 1u));
            fprintf(out, "\tmovzx %s, %s\n", gp_name(d, 4u), gp_name(d, 1u));
            return;
        }

        case MI_X64_CMOVCC:
            fprintf(out, "\tcmov%s %s, %s\n",
                    x64_cc_str(ins->ops[2].u.cond),
                    gp_name(ins->ops[0].u.preg, ins->width <= 4u ? 4u : 8u),
                    gp_name(ins->ops[1].u.preg, ins->width <= 4u ? 4u : 8u));
            return;

        case MI_X64_MFENCE:
            fprintf(out, "\tmfence\n");
            return;

        case MI_X64_ATOMIC_RMW: {
            PirRmwOp rop = (PirRmwOp)ins->aux_id;
            uint8_t w = ins->width;
            const char *q = ptr_qual(w);
            if (rop == PIR_RMW_XCHG) {
                /* ops[0] = dst_old (defuse initialized to val), ops[1] = ptr */
                fprintf(out, "\txchg %s [%s], %s\n",
                        q, gp_name(ins->ops[1].u.preg, 8u), gp_name(ins->ops[0].u.preg, w));
            } else if (rop == PIR_RMW_ADD || rop == PIR_RMW_SUB) {
                /* ops[0] = dst_old (defuse initialized to val or -val), ops[1] = ptr */
                fprintf(out, "\tlock xadd %s [%s], %s\n",
                        q, gp_name(ins->ops[1].u.preg, 8u), gp_name(ins->ops[0].u.preg, w));
            } else {
                /* AND / OR / XOR: CAS loop with fixed rax = old, ops[0]=ptr, ops[1]=val, ops[3]=tmp */
                uint32_t seq = ctx->local_seq++;
                const char *ptr = gp_name(ins->ops[0].u.preg, 8u);
                const char *val = gp_name(ins->ops[1].u.preg, w);
                const char *ra  = gp_name(X64_RAX, w);
                const char *rt  = gp_name(ins->ops[3].u.preg, w);
                const char *opm = (rop == PIR_RMW_AND) ? "and" : (rop == PIR_RMW_OR) ? "or" : "xor";
                fprintf(out, "\tmov %s, %s [%s]\n", ra, q, ptr);
                fprintf(out, "%sRMW_%u_%u:\n", lp, mf->fn_index, seq);
                fprintf(out, "\tmov %s, %s\n", rt, ra);
                fprintf(out, "\t%s %s, %s\n", opm, rt, val);
                fprintf(out, "\tlock cmpxchg %s [%s], %s\n", q, ptr, rt);
                fprintf(out, "\tjne %sRMW_%u_%u\n", lp, mf->fn_index, seq);
            }
            return;
        }

        case MI_X64_ATOMIC_CAS: {
            /* ops[0]=dst_ok, ops[1]=ptr, ops[2]=des, ops[3]=rax (defuse) */
            uint8_t w = ins->width;
            MPReg ok = ins->ops[0].u.preg;
            fprintf(out, "\tlock cmpxchg %s [%s], %s\n",
                    ptr_qual(w),
                    gp_name(ins->ops[1].u.preg, 8u),
                    gp_name(ins->ops[2].u.preg, w));
            fprintf(out, "\tsete %s\n", gp_name(ok, 1u));
            fprintf(out, "\tmovzx %s, %s\n", gp_name(ok, 4u), gp_name(ok, 1u));
            return;
        }

        /* SSE2 Scalar FP & Conversions */
        case MI_X64_MOVX_RR:
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fmt_xmm(x1, sizeof(x1), ins->ops[1].u.preg);
            fprintf(out, "\tmovaps %s, %s\n", x0, x1);
            return;

        case MI_X64_MOVX_RM:
            fmt_mem_base_disp(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.imm);
            emit_xmm_load_store(out, true, ins->ops[0].u.preg, mem, ins->width);
            return;

        case MI_X64_MOVX_RM_IDX:
            fmt_mem_idx(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.preg,
                        ins->aux_width, ins->ops[3].u.imm);
            emit_xmm_load_store(out, true, ins->ops[0].u.preg, mem, ins->width);
            return;

        case MI_X64_MOVX_MR:
            fmt_mem_base_disp(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.imm);
            emit_xmm_load_store(out, false, ins->ops[0].u.preg, mem, ins->width);
            return;

        case MI_X64_MOVX_MR_IDX:
            fmt_mem_idx(mem, sizeof(mem), ins->ops[1].u.preg, ins->ops[2].u.preg,
                        ins->aux_width, ins->ops[3].u.imm);
            emit_xmm_load_store(out, false, ins->ops[0].u.preg, mem, ins->width);
            return;

        case MI_X64_MOV_G2X:
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fprintf(out, "\t%s %s, %s\n",
                    ins->width <= 4u ? "movd" : "movq", x0,
                    gp_name(ins->ops[1].u.preg, ins->width <= 4u ? 4u : 8u));
            return;

        case MI_X64_MOV_X2G:
            fmt_xmm(x1, sizeof(x1), ins->ops[1].u.preg);
            fprintf(out, "\t%s %s, %s\n",
                    ins->width <= 4u ? "movd" : "movq",
                    gp_name(ins->ops[0].u.preg, ins->width <= 4u ? 4u : 8u), x1);
            return;

        case MI_X64_ADDSS: case MI_X64_ADDSD:
        case MI_X64_SUBSS: case MI_X64_SUBSD:
        case MI_X64_MULSS: case MI_X64_MULSD:
        case MI_X64_DIVSS: case MI_X64_DIVSD:
        case MI_X64_SQRTSS: case MI_X64_SQRTSD:
        case MI_X64_UCOMISS: case MI_X64_UCOMISD:
        case MI_X64_CVTSS2SD: case MI_X64_CVTSD2SS:
        case MI_X64_PADDB: case MI_X64_PADDW: case MI_X64_PADDD: case MI_X64_PADDQ:
        case MI_X64_PSUBB: case MI_X64_PSUBW: case MI_X64_PSUBD: case MI_X64_PSUBQ:
        case MI_X64_PMULLW:
        case MI_X64_PAND: case MI_X64_POR: case MI_X64_PXOR: case MI_X64_PANDN:
        case MI_X64_ADDPS: case MI_X64_ADDPD: case MI_X64_SUBPS: case MI_X64_SUBPD:
        case MI_X64_MULPS: case MI_X64_MULPD: case MI_X64_DIVPS: case MI_X64_DIVPD:
        case MI_X64_MINPS: case MI_X64_MINPD: case MI_X64_MAXPS: case MI_X64_MAXPD:
        case MI_X64_PCMPEQB: case MI_X64_PCMPEQW: case MI_X64_PCMPEQD:
        case MI_X64_PCMPGTB: case MI_X64_PCMPGTW: case MI_X64_PCMPGTD:
        case MI_X64_PUNPCKLBW: case MI_X64_PUNPCKLWD: case MI_X64_PUNPCKLDQ: case MI_X64_PUNPCKLQDQ:
        case MI_X64_CVTDQ2PS: case MI_X64_CVTTPS2DQ: {
            const char *m = (ins->op == MI_X64_ADDSS)     ? "addss"
                          : (ins->op == MI_X64_ADDSD)     ? "addsd"
                          : (ins->op == MI_X64_SUBSS)     ? "subss"
                          : (ins->op == MI_X64_SUBSD)     ? "subsd"
                          : (ins->op == MI_X64_MULSS)     ? "mulss"
                          : (ins->op == MI_X64_MULSD)     ? "mulsd"
                          : (ins->op == MI_X64_DIVSS)     ? "divss"
                          : (ins->op == MI_X64_DIVSD)     ? "divsd"
                          : (ins->op == MI_X64_SQRTSS)    ? "sqrtss"
                          : (ins->op == MI_X64_SQRTSD)    ? "sqrtsd"
                          : (ins->op == MI_X64_UCOMISS)   ? "ucomiss"
                          : (ins->op == MI_X64_UCOMISD)   ? "ucomisd"
                          : (ins->op == MI_X64_CVTSS2SD)  ? "cvtss2sd"
                          : (ins->op == MI_X64_CVTSD2SS)  ? "cvtsd2ss"
                          : (ins->op == MI_X64_PADDB)     ? "paddb"
                          : (ins->op == MI_X64_PADDW)     ? "paddw"
                          : (ins->op == MI_X64_PADDD)     ? "paddd"
                          : (ins->op == MI_X64_PADDQ)     ? "paddq"
                          : (ins->op == MI_X64_PSUBB)     ? "psubb"
                          : (ins->op == MI_X64_PSUBW)     ? "psubw"
                          : (ins->op == MI_X64_PSUBD)     ? "psubd"
                          : (ins->op == MI_X64_PSUBQ)     ? "psubq"
                          : (ins->op == MI_X64_PMULLW)    ? "pmullw"
                          : (ins->op == MI_X64_PAND)      ? "pand"
                          : (ins->op == MI_X64_POR)       ? "por"
                          : (ins->op == MI_X64_PXOR)      ? "pxor"
                          : (ins->op == MI_X64_PANDN)     ? "pandn"
                          : (ins->op == MI_X64_ADDPS)     ? "addps"
                          : (ins->op == MI_X64_ADDPD)     ? "addpd"
                          : (ins->op == MI_X64_SUBPS)     ? "subps"
                          : (ins->op == MI_X64_SUBPD)     ? "subpd"
                          : (ins->op == MI_X64_MULPS)     ? "mulps"
                          : (ins->op == MI_X64_MULPD)     ? "mulpd"
                          : (ins->op == MI_X64_DIVPS)     ? "divps"
                          : (ins->op == MI_X64_DIVPD)     ? "divpd"
                          : (ins->op == MI_X64_MINPS)     ? "minps"
                          : (ins->op == MI_X64_MINPD)     ? "minpd"
                          : (ins->op == MI_X64_MAXPS)     ? "maxps"
                          : (ins->op == MI_X64_MAXPD)     ? "maxpd"
                          : (ins->op == MI_X64_PCMPEQB)   ? "pcmpeqb"
                          : (ins->op == MI_X64_PCMPEQW)   ? "pcmpeqw"
                          : (ins->op == MI_X64_PCMPEQD)   ? "pcmpeqd"
                          : (ins->op == MI_X64_PCMPGTB)   ? "pcmpgtb"
                          : (ins->op == MI_X64_PCMPGTW)   ? "pcmpgtw"
                          : (ins->op == MI_X64_PCMPGTD)   ? "pcmpgtd"
                          : (ins->op == MI_X64_PUNPCKLBW) ? "punpcklbw"
                          : (ins->op == MI_X64_PUNPCKLWD) ? "punpcklwd"
                          : (ins->op == MI_X64_PUNPCKLDQ) ? "punpckldq"
                          : (ins->op == MI_X64_PUNPCKLQDQ)? "punpcklqdq"
                          : (ins->op == MI_X64_CVTDQ2PS)  ? "cvtdq2ps" : "cvttps2dq";
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fmt_xmm(x1, sizeof(x1), ins->op_count >= 2u ? ins->ops[1].u.preg : ins->ops[0].u.preg);
            fprintf(out, "\t%s %s, %s\n", m, x0, x1);
            return;
        }

        case MI_X64_CVTSI2SS:
        case MI_X64_CVTSI2SD:
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fprintf(out, "\t%s %s, %s\n",
                    ins->op == MI_X64_CVTSI2SS ? "cvtsi2ss" : "cvtsi2sd",
                    x0, gp_name(ins->ops[1].u.preg, ins->aux_width <= 4u ? 4u : 8u));
            return;

        case MI_X64_CVTTSS2SI:
        case MI_X64_CVTTSD2SI:
            fmt_xmm(x1, sizeof(x1), ins->ops[1].u.preg);
            fprintf(out, "\t%s %s, %s\n",
                    ins->op == MI_X64_CVTTSS2SI ? "cvttss2si" : "cvttsd2si",
                    gp_name(ins->ops[0].u.preg, ins->width <= 4u ? 4u : 8u), x1);
            return;

        case MI_X64_CMPPS:
        case MI_X64_CMPPD:
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fmt_xmm(x1, sizeof(x1), ins->ops[1].u.preg);
            fprintf(out, "\t%s %s, %s, %u\n",
                    ins->op == MI_X64_CMPPS ? "cmpps" : "cmppd",
                    x0, x1, (unsigned)ins->aux_width);
            return;

        case MI_X64_PSHUFD:
            fmt_xmm(x0, sizeof(x0), ins->ops[0].u.preg);
            fmt_xmm(x1, sizeof(x1), ins->ops[1].u.preg);
            fprintf(out, "\tpshufd %s, %s, %u\n", x0, x1, (unsigned)ins->aux_width);
            return;

        /* Control Flow */
        case MI_X64_JMP:
            fprintf(out, "\tjmp %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, ins->ops[0].u.block_id));
            return;

        case MI_X64_JCC:
            fprintf(out, "\tj%s %sBB%u_%u\n",
                    x64_cc_str(ins->ops[0].u.cond),
                    lp, mf->fn_index, redir_blk(ctx, ins->ops[1].u.block_id));
            return;

        case MI_X64_CALL:
            fputs("\tcall ", out);
            x64_emit_sym(out, target, ins->ops[0].u.sym.name);
            fputc('\n', out);
            return;

        case MI_X64_CALL_IND:
            fprintf(out, "\tcall %s\n", gp_name(ins->ops[0].u.preg, 8u));
            return;

        case MI_X64_SWITCH_TBL: {
            const char *vr = gp_name(ins->ops[0].u.preg, 8u);
            uint32_t jtid = ins->ops[1].u.jtable_id;
            const MJTable *jt = &mf->jtables.data[jtid];
            /* r10 = val - min_val */
            if (jt->min_val == 0) {
                fprintf(out, "\tmov r10, %s\n", vr);
            } else if (jt->min_val >= INT32_MIN && jt->min_val <= INT32_MAX) {
                fprintf(out, "\tmov r10, %s\n", vr);
                fprintf(out, "\tsub r10, %" PRId64 "\n", jt->min_val);
            } else {
                fprintf(out, "\tmovabs r11, %" PRId64 "\n", jt->min_val);
                fprintf(out, "\tmov r10, %s\n", vr);
                fprintf(out, "\tsub r10, r11\n");
            }
            fprintf(out, "\tcmp r10, %u\n", jt->num_entries - 1u);
            fprintf(out, "\tja %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, jt->default_blk));
            fprintf(out, "\tlea r11, [rip + %sJT%u_%u]\n", lp, mf->fn_index, jtid);
            fprintf(out, "\tmovsxd r10, dword ptr [r11 + r10*4]\n");
            fprintf(out, "\tadd r10, r11\n");
            fprintf(out, "\tjmp r10\n");
            return;
        }

        case MI_X64_RET:
            fprintf(out, "\tret\n");
            return;

        case MI_X64_UD2:
            fprintf(out, "\tud2\n");
            return;

        /* Pseudo-ops & ARM64 opcodes must not reach x64_asm */
        case MI_COPY: case MI_PCOPY: case MI_FRAME_ADDR:
        case MI_LOAD_SLOT: case MI_STORE_SLOT:
        case MI_LOAD_INARG: case MI_INARG_ADDR:
        case MI_STORE_OUTARG: case MI_COPY_OUTARG_MEM:
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
            fprintf(stderr, "internal error: unexpected MIR op %u in x64_asm\n", (unsigned)ins->op);
            abort();
    }
}

static bool x64_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const PirFunction *fn = mf->pir_fn;
    const char *lp = pir_asm_lbl_pfx(target);

    pir_asm_emit_func_prologue(out, mod, target, mf, 4u, "@", x64_needs_sym_quote);

    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = NULL;
    bool     *blk_skip  = NULL;
    pir_asm_build_blk_redir(mf, MI_X64_JMP, &blk_redir, &blk_skip);
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
            if (j + 2u == blk->insts.len &&
                ins->op == MI_X64_JCC &&
                blk->insts.data[j + 1u].op == MI_X64_JMP) {
                uint32_t tdst = redir_blk(ctx, ins->ops[1].u.block_id);
                uint32_t fdst = redir_blk(ctx, blk->insts.data[j + 1u].ops[0].u.block_id);
                if (tdst == next_live) {
                    if (fdst != next_live) {
                        fprintf(out, "\tj%s %sBB%u_%u\n",
                                x64_cc_str(pir_asm_invert_cond(ins->ops[0].u.cond)),
                                lp, mf->fn_index, fdst);
                    }
                    break;
                }
                if (fdst == b && tdst != b) {
                    fprintf(out, "\tj%s %sBB%u_%u\n",
                            x64_cc_str(pir_asm_invert_cond(ins->ops[0].u.cond)),
                            lp, mf->fn_index, b);
                    fprintf(out, "\tjmp %sBB%u_%u\n", lp, mf->fn_index, tdst);
                    break;
                }
            }
            if (ins->op == MI_X64_JMP && redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            pir_asm_emit_dwarf_loc(out, mod, fn, &loc_st, ins->loc_idx);
            emit_inst(ctx, mf, ins);
        }
    }

    pir_asm_emit_jtables(out, target, mf, blk_redir);

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);

    pir_asm_emit_func_epilogue(out, mod, target, mf, x64_needs_sym_quote);
    return true;
}

static bool x64_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    return pir_asm_emit_globals(ctx->out, mod, target, x64_needs_sym_quote);
}

static bool x64_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    return pir_asm_emit_module_footer(ctx->out, mod, target);
}

void pir_x64_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)calloc(1u, sizeof(X64AsmCtx));
    if (!ctx) abort();
    ctx->out    = out;
    ctx->target = target;

    em->ctx                = ctx;
    em->emit_module_header = x64_emit_header;
    em->emit_function      = x64_emit_function;
    em->emit_globals       = x64_emit_globals;
    em->emit_module_footer = x64_emit_footer;
}
