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

#include "lower/lower.h"

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

static bool is_macho(const PirTargetSpec *target) {
    return target->os == PIR_OS_MACOS;
}

static const char *sym_pfx(const PirTargetSpec *target) {
    return is_macho(target) ? "_" : "";
}

static bool is_bare_asm_sym_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c == '.';
}

static const char *fmt_asm_sym(char *buf, size_t cap, const PirTargetSpec *target,
                               const char *name, const char *suffix) {
    const char *sp = sym_pfx(target);
    bool bare = (name[0] != '\0');
    for (const char *p = name; *p && bare; ++p) {
        if (!is_bare_asm_sym_char((unsigned char)*p)) bare = false;
    }
    if (bare) {
        snprintf(buf, cap, "%s%s%s", sp, name, suffix ? suffix : "");
        return buf;
    }
    size_t pos = 0u;
    if (pos + 2u < cap) buf[pos++] = '"';
    for (const char *p = sp; *p && pos + 2u < cap; ++p) buf[pos++] = *p;
    for (const char *p = name; *p && pos + 3u < cap; ++p) {
        if (*p == '"' || *p == '\\') buf[pos++] = '\\';
        buf[pos++] = *p;
    }
    if (suffix) {
        for (const char *p = suffix; *p && pos + 2u < cap; ++p) buf[pos++] = *p;
    }
    if (pos + 1u < cap) buf[pos++] = '"';
    buf[pos] = '\0';
    return buf;
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

static bool x64_emit_header(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    FILE *out = ctx->out;

    if (is_macho(target)) {
        fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
    } else {
        fprintf(out, "\t.text\n");
    }
    fprintf(out, "\t.intel_syntax noprefix\n");
    for (uint32_t i = 0u; i < mod->files.len; ++i) {
        fprintf(out, "\t.file %u \"%s\"\n", mod->files.data[i].id + 1u, mod->files.data[i].path);
    }
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
    const char *lp = lbl_pfx(target);
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
            if (ins->aux_id == 0u) fprintf(out, "\tmov %s, rsp\n", gp_name(ins->ops[0].u.preg, 8u));
            else                   fprintf(out, "\tmov rsp, %s\n", gp_name(ins->ops[0].u.preg, 8u));
            return;

        case MI_X64_SYM_ADDR: {
            const char *dst = gp_name(ins->ops[0].u.preg, 8u);
            const char *name = ins->ops[1].u.sym.name;
            bool use_got     = ins->ops[1].u.sym.use_got;
            char sbuf[512];
            const char *asym = fmt_asm_sym(sbuf, sizeof(sbuf), target, name, NULL);
            if (use_got) {
                fprintf(out, "\tmov %s, qword ptr [rip + %s@GOTPCREL]\n", dst, asym);
            } else {
                fprintf(out, "\tlea %s, [rip + %s]\n", dst, asym);
            }
            return;
        }

        case MI_X64_TLS_ADDR: {
            const char *name = ins->ops[1].u.sym.name;
            char sbuf[512];
            const char *asym = fmt_asm_sym(sbuf, sizeof(sbuf), target, name, NULL);
            if (is_macho(target)) {
                fprintf(out, "\tmov rdi, qword ptr [rip + %s@TLVP]\n", asym);
                fprintf(out, "\tcall qword ptr [rdi]\n");
            } else {
                const char *dst = gp_name(ins->ops[0].u.preg, 8u);
                fprintf(out, "\tmov %s, qword ptr fs:[0]\n", dst);
                fprintf(out, "\tlea %s, [%s + %s@TPOFF]\n", dst, dst, asym);
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

        case MI_X64_CALL: {
            char sbuf[512];
            fprintf(out, "\tcall %s\n", fmt_asm_sym(sbuf, sizeof(sbuf), target, ins->ops[0].u.sym.name, NULL));
            return;
        }

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
    const char *lp = lbl_pfx(target);
    char fn_sbuf[512];
    const char *fn_asym = fmt_asm_sym(fn_sbuf, sizeof(fn_sbuf), target, fn->name, NULL);

    if (fn->section && fn->section[0] != '\0') {
        emit_section_directive(out, target, fn->section, PIR_GSEG_RODATA, false, false);
    } else if (is_macho(target)) {
        fprintf(out, "\t.section __TEXT,__text,regular,pure_instructions\n");
    } else {
        fprintf(out, "\t.text\n");
    }

    if (fn->linkage == PIR_LINK_EXPORT) {
        fprintf(out, "\t.globl %s\n", fn_asym);
    } else if (fn->linkage == PIR_LINK_WEAK) {
        if (is_macho(target)) {
            fprintf(out, "\t.globl %s\n", fn_asym);
            fprintf(out, "\t.weak_definition %s\n", fn_asym);
        } else {
            fprintf(out, "\t.weak %s\n", fn_asym);
        }
    }
    if (!is_macho(target)) {
        fprintf(out, "\t.type %s, @function\n", fn_asym);
    }
    fprintf(out, "\t.p2align 4\n");
    fprintf(out, "%s:\n", fn_asym);
    if (mod->files.len > 0u && is_macho(target)) {
        fprintf(out, "%sfunc_begin%u:\n", lp, mf->fn_index);
    }
    fprintf(out, "\t.cfi_startproc\n");

    uint32_t nblks = mf->blocks.len;
    uint32_t *blk_redir = nblks ? (uint32_t *)malloc((size_t)nblks * sizeof(uint32_t)) : NULL;
    bool     *blk_skip  = nblks ? (bool *)calloc((size_t)nblks, sizeof(bool)) : NULL;
    uint32_t *vis_mark  = nblks ? (uint32_t *)calloc((size_t)nblks, sizeof(uint32_t)) : NULL;
    if (nblks && (!blk_redir || !blk_skip || !vis_mark)) abort();
    for (uint32_t b = 0u; b < nblks; ++b) blk_redir[b] = b;
    for (uint32_t b = 1u; b < nblks; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        if (blk->insts.len == 1u && blk->insts.data[0].op == MI_X64_JMP) {
            uint32_t dst = blk->insts.data[0].ops[0].u.block_id;
            if (dst < nblks && dst != b) blk_redir[b] = dst;
        }
    }
    for (uint32_t b = 1u; b < nblks; ++b) {
        uint32_t cur = b;
        while (blk_redir[cur] != cur) {
            if (vis_mark[cur] == b) {
                blk_redir[cur] = cur;
                break;
            }
            if (vis_mark[cur] != 0u) break;
            vis_mark[cur] = b;
            cur = blk_redir[cur];
        }
    }
    free(vis_mark);
    for (uint32_t b = 1u; b < nblks; ++b) {
        uint32_t cur = blk_redir[b];
        while (blk_redir[cur] != cur) cur = blk_redir[cur];
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
            if (j + 2u == blk->insts.len &&
                ins->op == MI_X64_JCC &&
                blk->insts.data[j + 1u].op == MI_X64_JMP) {
                uint32_t tdst = redir_blk(ctx, ins->ops[1].u.block_id);
                uint32_t fdst = redir_blk(ctx, blk->insts.data[j + 1u].ops[0].u.block_id);
                if (tdst == next_live) {
                    if (fdst != next_live) {
                        fprintf(out, "\tj%s %sBB%u_%u\n",
                                x64_cc_str(invert_cond(ins->ops[0].u.cond)),
                                lp, mf->fn_index, fdst);
                    }
                    break;
                }
                if (fdst == b && tdst != b) {
                    fprintf(out, "\tj%s %sBB%u_%u\n",
                            x64_cc_str(invert_cond(ins->ops[0].u.cond)),
                            lp, mf->fn_index, b);
                    fprintf(out, "\tjmp %sBB%u_%u\n", lp, mf->fn_index, tdst);
                    break;
                }
            }
            if (ins->op == MI_X64_JMP && redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
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
        fprintf(out, "\t.size %s, .-%s\n", fn_asym, fn_asym);
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
    if (g->is_zeroinit) {
        uint64_t sz = compute_global_bytes(mod, target, g);
        fprintf(out, "\t.zero %" PRIu64 "\n", sz);
        return;
    }
    if (g->has_type && !g->is_braced && g->item_count == 1u) {
        const PirGlobalItem *gi = &g->items[0];
        PirTypeLayout ly = pir_type_layout(mod, target, g->type);
        if (gi->kind == PIR_GITEM_INT) {
            uint64_t x = gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo;
            if (ly.size == 1u)      fprintf(out, "\t.byte %" PRIu64 "\n", x & 0xFFu);
            else if (ly.size == 2u) fprintf(out, "\t.short %" PRIu64 "\n", x & 0xFFFFu);
            else if (ly.size == 4u) fprintf(out, "\t.long %" PRIu64 "\n", x & 0xFFFFFFFFu);
            else                    fprintf(out, "\t.quad %" PRIu64 "\n", x);
        } else if (gi->kind == PIR_GITEM_FLOAT) {
            if (ly.size == 4u) fprintf(out, "\t.long %" PRIu64 "\n", gi->float_bits & 0xFFFFFFFFu);
            else               fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
        } else if (gi->kind == PIR_GITEM_RELOC) {
            char sbuf[512];
            const char *asym = fmt_asm_sym(sbuf, sizeof(sbuf), target, gi->sym_name, NULL);
            if (gi->sym_offset != 0)
                fprintf(out, "\t.quad %s + %" PRId64 "\n", asym, gi->sym_offset);
            else
                fprintf(out, "\t.quad %s\n", asym);
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
                uint8_t b = (uint8_t)(gi->int_neg ? (UINT64_C(0) - gi->int_lo) : gi->int_lo);
                fprintf(out, "\t.byte %u\n", (unsigned)b);
                break;
            }
            case PIR_GITEM_FLOAT:
                fprintf(out, "\t.quad %" PRIu64 "\n", gi->float_bits);
                break;
            case PIR_GITEM_RELOC: {
                char sbuf[512];
                const char *asym = fmt_asm_sym(sbuf, sizeof(sbuf), target, gi->sym_name, NULL);
                if (gi->sym_offset != 0)
                    fprintf(out, "\t.quad %s + %" PRId64 "\n", asym, gi->sym_offset);
                else
                    fprintf(out, "\t.quad %s\n", asym);
                break;
            }
        }
    }
}

static bool x64_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    FILE *out = ctx->out;

    for (uint32_t i = 0u; i < mod->globals.len; ++i) {
        const PirGlobal *g = &mod->globals.data[i];
        if (g->linkage == PIR_LINK_EXTERN) continue;
        uint32_t al = g->align > 0u ? g->align : 1u;
        uint32_t p2 = log2_align(al);
        bool all_zero = global_is_all_zero(g);
        uint64_t sz = compute_global_bytes(mod, target, g);
        char g_sbuf[512], t_sbuf[512];
        const char *g_asym = fmt_asm_sym(g_sbuf, sizeof(g_sbuf), target, g->name, NULL);

        if (is_macho(target) && g->seg == PIR_GSEG_TLS) {
            const char *t_asym = fmt_asm_sym(t_sbuf, sizeof(t_sbuf), target, g->name, "$tlv$init");
            if (all_zero) {
                fprintf(out, "\t.tbss %s, %" PRIu64 ", %u\n", t_asym, sz, p2);
            } else {
                fprintf(out, "\t.section __DATA,__thread_data,thread_local_regular\n");
                if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
                fprintf(out, "%s:\n", t_asym);
                emit_global_data_items(out, mod, target, g);
            }
            fprintf(out, "\t.section __DATA,__thread_vars,thread_local_variables\n");
            if (g->linkage == PIR_LINK_EXPORT) {
                fprintf(out, "\t.globl %s\n", g_asym);
            } else if (g->linkage == PIR_LINK_WEAK) {
                fprintf(out, "\t.globl %s\n", g_asym);
                fprintf(out, "\t.weak_definition %s\n", g_asym);
            }
            fprintf(out, "%s:\n", g_asym);
            fprintf(out, "\t.quad __tlv_bootstrap\n");
            fprintf(out, "\t.quad 0\n");
            fprintf(out, "\t.quad %s\n\n", t_asym);
            continue;
        }

        if (is_macho(target) && g->seg == PIR_GSEG_DATA && all_zero &&
            (!g->section || g->section[0] == '\0') && g->linkage == PIR_LINK_INTERNAL) {
            fprintf(out, "\t.zerofill __DATA,__bss,%s,%" PRIu64 ",%u\n\n", g_asym, sz, p2);
            continue;
        }

        bool has_rel = global_has_reloc(g);
        emit_section_directive(out, target, g->section, g->seg, has_rel, all_zero);

        if (g->linkage == PIR_LINK_EXPORT) {
            fprintf(out, "\t.globl %s\n", g_asym);
        } else if (g->linkage == PIR_LINK_WEAK) {
            if (is_macho(target)) {
                fprintf(out, "\t.globl %s\n", g_asym);
                fprintf(out, "\t.weak_definition %s\n", g_asym);
            } else {
                fprintf(out, "\t.weak %s\n", g_asym);
            }
        }
        if (p2 > 0u) fprintf(out, "\t.p2align %u\n", p2);
        fprintf(out, "%s:\n", g_asym);
        emit_global_data_items(out, mod, target, g);
        fprintf(out, "\n");
    }
    return true;
}

static bool x64_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                            const PirTargetSpec *target) {
    X64AsmCtx *ctx = (X64AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const char *lp = lbl_pfx(target);

    if (mod->files.len > 0u && is_macho(target)) {
        fprintf(out, "\t.section __DWARF,__debug_abbrev,regular,debug\n");
        fprintf(out, "%spir_abbrev:\n", lp);
        fprintf(out, "\t.byte 1, 17, 1\n");
        fprintf(out, "\t.byte 3, 8\n");
        fprintf(out, "\t.byte 27, 8\n");
        fprintf(out, "\t.byte 16, 23\n");
        fprintf(out, "\t.byte 0, 0\n");
        fprintf(out, "\t.byte 2, 46, 0\n");
        fprintf(out, "\t.byte 17, 1\n");
        fprintf(out, "\t.byte 18, 6\n");
        fprintf(out, "\t.byte 3, 8\n");
        fprintf(out, "\t.byte 0, 0\n");
        fprintf(out, "\t.byte 0\n");

        fprintf(out, "\t.section __DWARF,__debug_info,regular,debug\n");
        fprintf(out, "\t.long %spir_cu_end - %spir_cu_start\n", lp, lp);
        fprintf(out, "%spir_cu_start:\n", lp);
        fprintf(out, "\t.short 4\n");
        fprintf(out, "\t.long 0\n");
        fprintf(out, "\t.byte 8\n");
        fprintf(out, "\t.byte 1\n");
        fprintf(out, "\t.asciz \"%s\"\n", mod->files.data[0].path);
        fprintf(out, "\t.asciz \".\"\n");
        fprintf(out, "\t.long 0\n");
        for (uint32_t fi = 0u; fi < mod->funcs.len; ++fi) {
            const PirFunction *f = &mod->funcs.data[fi];
            if (!f->is_defined) continue;
            fprintf(out, "\t.byte 2\n");
            fprintf(out, "\t.quad %sfunc_begin%u\n", lp, fi);
            fprintf(out, "\t.long %sfunc_end%u - %sfunc_begin%u\n", lp, fi, lp, fi);
            fprintf(out, "\t.asciz \"%s\"\n", f->name);
        }
        fprintf(out, "\t.byte 0\n");
        fprintf(out, "%spir_cu_end:\n", lp);
    }

    if (is_macho(target)) {
        fprintf(out, ".subsections_via_symbols\n");
    } else {
        fprintf(out, "\t.section .note.GNU-stack,\"\",@progbits\n");
    }
    return true;
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
