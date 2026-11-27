/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * RISC-V 32-bit (RV32IMAFD) GAS/LLVM-MC Assembly Emitter (ELF).
 */

#include "backend/backend.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend/asm_common.h"

typedef struct {
    FILE                *out;
    const PirTargetSpec *target;
    uint32_t             local_seq;
    const uint32_t      *blk_redir;
    bool                 need_branch_relax;
    PIR_VEC(uint64_t)    f64_pool;
} Rv32AsmCtx;

static uint32_t redir_blk(const Rv32AsmCtx *ctx, uint32_t bid) {
    return ctx->blk_redir ? ctx->blk_redir[bid] : bid;
}

static const char *RV32_GP_NAMES[32] = {
    "zero", "ra", "sp",  "gp",  "tp", "t0", "t1", "t2",
    "s0",   "s1", "a0",  "a1",  "a2", "a3", "a4", "a5",
    "a6",   "a7", "s2",  "s3",  "s4", "s5", "s6", "s7",
    "s8",   "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

static const char *RV32_FP_NAMES[32] = {
    "ft0", "ft1", "ft2",  "ft3",  "ft4", "ft5", "ft6",  "ft7",
    "fs0", "fs1", "fa0",  "fa1",  "fa2", "fa3", "fa4",  "fa5",
    "fa6", "fa7", "fs2",  "fs3",  "fs4", "fs5", "fs6",  "fs7",
    "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"
};

static const char *gp_name(MPReg p) {
    return RV32_GP_NAMES[rv32_reg_num(p) & 31u];
}

static const char *fp_name(MPReg p) {
    return RV32_FP_NAMES[rv32_reg_num(p) & 31u];
}

static bool rv32_needs_sym_quote(const char *s) {
    if (!s || !s[0]) return true;
    if (strcmp(s, "fp") == 0) return true;
    for (uint32_t i = 0u; i < 32u; ++i) {
        if (strcmp(s, RV32_GP_NAMES[i]) == 0 || strcmp(s, RV32_FP_NAMES[i]) == 0) {
            return true;
        }
    }
    if ((s[0] == 'x' || s[0] == 'f') && s[1] >= '0' && s[1] <= '9') {
        if (s[2] == '\0') return true;
        if (s[2] >= '0' && s[2] <= '9' && s[3] == '\0') {
            uint32_t v = (uint32_t)(s[1] - '0') * 10u + (uint32_t)(s[2] - '0');
            return v <= 31u && s[1] != '0';
        }
    }
    return false;
}

static bool rv32_is_bare_call_sym(const char *s) {
    if (!s || !s[0]) return false;
    unsigned char c0 = (unsigned char)s[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_' || c0 == '.')) {
        return false;
    }
    for (const char *p = s + 1; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$')) {
            return false;
        }
    }
    return true;
}

static void rv32_emit_sym(FILE *out, const PirTargetSpec *target, const char *name) {
    pir_asm_emit_sym(out, target, name, NULL, rv32_needs_sym_quote);
}

static uint32_t add_f64_const(Rv32AsmCtx *ctx, uint64_t bits) {
    for (uint32_t i = 0u; i < ctx->f64_pool.len; ++i) {
        if (ctx->f64_pool.data[i] == bits) return i;
    }
    uint32_t id = ctx->f64_pool.len;
    PIR_VEC_PUSH(&ctx->f64_pool, bits);
    return id;
}

static void emit_bcc(Rv32AsmCtx *ctx, const MFunc *mf, MCond c,
                     const char *rs1, const char *rs2, uint32_t dst_blk) {
    FILE *out = ctx->out;
    const char *lp = pir_asm_lbl_pfx(ctx->target);
    const char *mn = "beq";
    const char *inv = "bne";
    bool swap = false;

    switch (c) {
        case MCOND_EQ: mn = "beq";  inv = "bne";  break;
        case MCOND_NE: mn = "bne";  inv = "beq";  break;
        case MCOND_LT: mn = "blt";  inv = "bge";  break;
        case MCOND_GE: mn = "bge";  inv = "blt";  break;
        case MCOND_GT: mn = "blt";  inv = "bge";  swap = true; break;
        case MCOND_LE: mn = "bge";  inv = "blt";  swap = true; break;
        case MCOND_CC: mn = "bltu"; inv = "bgeu"; break;
        case MCOND_CS: mn = "bgeu"; inv = "bltu"; break;
        case MCOND_HI: mn = "bltu"; inv = "bgeu"; swap = true; break;
        case MCOND_LS: mn = "bgeu"; inv = "bltu"; swap = true; break;
        case MCOND_MI: case MCOND_PL: case MCOND_VS: case MCOND_VC:
        case MCOND_P:  case MCOND_NP:
            mn = "bne"; inv = "beq"; break;
    }
    const char *a = swap ? rs2 : rs1;
    const char *b = swap ? rs1 : rs2;
    if (ctx->need_branch_relax) {
        uint32_t seq = ctx->local_seq++;
        fprintf(out, "\t%s %s, %s, %sBRX%u\n", inv, a, b, lp, seq);
        fprintf(out, "\tj %sBB%u_%u\n", lp, mf->fn_index, dst_blk);
        fprintf(out, "%sBRX%u:\n", lp, seq);
    } else {
        fprintf(out, "\t%s %s, %s, %sBB%u_%u\n", mn, a, b, lp, mf->fn_index, dst_blk);
    }
}

static bool rv32_emit_header(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    Rv32AsmCtx *ctx = (Rv32AsmCtx *)em->ctx;
    pir_asm_emit_section_directive(ctx->out, mod, target, NULL, PIR_GSEG_RODATA, false, false, true);
    pir_asm_emit_dwarf_files(ctx->out, mod);
    return true;
}

static void emit_inst(Rv32AsmCtx *ctx, const MFunc *mf, const MInst *ins) {
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    switch (ins->op) {
        case MI_NOP:
            return;

        case MI_RV32_LI:
            fprintf(out, "\tli %s, %" PRId64 "\n", gp_name(ins->ops[0].u.preg), ins->ops[1].u.imm);
            return;

        case MI_RV32_MV:
            if (ins->ops[0].u.preg != ins->ops[1].u.preg) {
                fprintf(out, "\tmv %s, %s\n", gp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            }
            return;

        case MI_RV32_MOV_SP:
            if (ins->aux_id == 0u) {
                fprintf(out, "\tmv %s, sp\n", gp_name(ins->ops[0].u.preg));
            } else {
                fprintf(out, "\tmv sp, %s\n", gp_name(ins->ops[0].u.preg));
            }
            return;

        case MI_RV32_SYM_ADDR: {
            const char *rd = gp_name(ins->ops[0].u.preg);
            const char *op = (ins->ops[1].u.sym.use_got && target->os == PIR_OS_LINUX) ? "la" : "lla";
            fprintf(out, "\t%s %s, ", op, rd);
            rv32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputc('\n', out);
            return;
        }

        case MI_RV32_TLS_ADDR: {
            const char *rd = gp_name(ins->ops[0].u.preg);
            fprintf(out, "\tlui %s, %%tprel_hi(", rd);
            rv32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs(")\n", out);
            fprintf(out, "\tadd %s, %s, tp, %%tprel_add(", rd, rd);
            rv32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs(")\n", out);
            fprintf(out, "\taddi %s, %s, %%tprel_lo(", rd, rd);
            rv32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs(")\n", out);
            return;
        }

        case MI_RV32_LD: {
            const char *mn = (ins->width == 1u) ? (ins->is_signed ? "lb" : "lbu")
                           : (ins->width == 2u) ? (ins->is_signed ? "lh" : "lhu") : "lw";
            fprintf(out, "\t%s %s, %" PRId64 "(%s)\n", mn,
                    gp_name(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_name(ins->ops[1].u.preg));
            return;
        }

        case MI_RV32_ST: {
            const char *mn = (ins->width == 1u) ? "sb" : (ins->width == 2u) ? "sh" : "sw";
            fprintf(out, "\t%s %s, %" PRId64 "(%s)\n", mn,
                    gp_name(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_name(ins->ops[1].u.preg));
            return;
        }

        case MI_RV32_FLD:
            fprintf(out, "\t%s %s, %" PRId64 "(%s)\n", ins->width == 4u ? "flw" : "fld",
                    fp_name(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FST:
            fprintf(out, "\t%s %s, %" PRId64 "(%s)\n", ins->width == 4u ? "fsw" : "fsd",
                    fp_name(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_ADD:   case MI_RV32_SUB:
        case MI_RV32_MUL:   case MI_RV32_MULH:  case MI_RV32_MULHU:
        case MI_RV32_DIV:   case MI_RV32_DIVU:  case MI_RV32_REM:   case MI_RV32_REMU:
        case MI_RV32_AND:   case MI_RV32_OR:    case MI_RV32_XOR:
        case MI_RV32_SLL:   case MI_RV32_SRL:   case MI_RV32_SRA:
        case MI_RV32_SLT:   case MI_RV32_SLTU: {
            const char *mn = (ins->op == MI_RV32_ADD)   ? "add"
                           : (ins->op == MI_RV32_SUB)   ? "sub"
                           : (ins->op == MI_RV32_MUL)   ? "mul"
                           : (ins->op == MI_RV32_MULH)  ? "mulh"
                           : (ins->op == MI_RV32_MULHU) ? "mulhu"
                           : (ins->op == MI_RV32_DIV)   ? "div"
                           : (ins->op == MI_RV32_DIVU)  ? "divu"
                           : (ins->op == MI_RV32_REM)   ? "rem"
                           : (ins->op == MI_RV32_REMU)  ? "remu"
                           : (ins->op == MI_RV32_AND)   ? "and"
                           : (ins->op == MI_RV32_OR)    ? "or"
                           : (ins->op == MI_RV32_XOR)   ? "xor"
                           : (ins->op == MI_RV32_SLL)   ? "sll"
                           : (ins->op == MI_RV32_SRL)   ? "srl"
                           : (ins->op == MI_RV32_SRA)   ? "sra"
                           : (ins->op == MI_RV32_SLT)   ? "slt" : "sltu";
            fprintf(out, "\t%s %s, %s, %s\n", mn,
                    gp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg), gp_name(ins->ops[2].u.preg));
            return;
        }

        case MI_RV32_ADDI: case MI_RV32_ANDI: case MI_RV32_ORI: case MI_RV32_XORI:
        case MI_RV32_SLLI: case MI_RV32_SRLI: case MI_RV32_SRAI:
        case MI_RV32_SLTI: case MI_RV32_SLTIU: {
            const char *mn = (ins->op == MI_RV32_ADDI)  ? "addi"
                           : (ins->op == MI_RV32_ANDI)  ? "andi"
                           : (ins->op == MI_RV32_ORI)   ? "ori"
                           : (ins->op == MI_RV32_XORI)  ? "xori"
                           : (ins->op == MI_RV32_SLLI)  ? "slli"
                           : (ins->op == MI_RV32_SRLI)  ? "srli"
                           : (ins->op == MI_RV32_SRAI)  ? "srai"
                           : (ins->op == MI_RV32_SLTI)  ? "slti" : "sltiu";
            fprintf(out, "\t%s %s, %s, %" PRId64 "\n", mn,
                    gp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg), ins->ops[2].u.imm);
            return;
        }

        case MI_RV32_SEQZ:
            fprintf(out, "\tseqz %s, %s\n", gp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_SNEZ:
            fprintf(out, "\tsnez %s, %s\n", gp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FENCE:
            switch ((PirMemOrder)ins->aux_id) {
                case PIR_ORDER_ACQUIRE: fputs("\tfence r, rw\n", out); break;
                case PIR_ORDER_RELEASE: fputs("\tfence rw, w\n", out); break;
                case PIR_ORDER_NONE:
                case PIR_ORDER_RELAXED: break;
                case PIR_ORDER_ACQ_REL:
                case PIR_ORDER_SEQ_CST: fputs("\tfence rw, rw\n", out); break;
            }
            return;

        case MI_RV32_ATOMIC_RMW: {
            const char *rd  = gp_name(ins->ops[0].u.preg);
            const char *ptr = gp_name(ins->ops[1].u.preg);
            const char *val = gp_name(ins->ops[2].u.preg);
            uint32_t seq = ctx->local_seq++;
            PirRmwOp rop = (PirRmwOp)ins->aux_id;
            if (ins->width == 4u) {
                fprintf(out, "%sARM_%u:\n", lp, seq);
                fprintf(out, "\tlr.w.aqrl %s, (%s)\n", rd, ptr);
                switch (rop) {
                    case PIR_RMW_ADD:  fprintf(out, "\tadd t5, %s, %s\n", rd, val); break;
                    case PIR_RMW_SUB:  fprintf(out, "\tsub t5, %s, %s\n", rd, val); break;
                    case PIR_RMW_AND:  fprintf(out, "\tand t5, %s, %s\n", rd, val); break;
                    case PIR_RMW_OR:   fprintf(out, "\tor t5, %s, %s\n",  rd, val); break;
                    case PIR_RMW_XOR:  fprintf(out, "\txor t5, %s, %s\n", rd, val); break;
                    case PIR_RMW_XCHG: fprintf(out, "\tmv t5, %s\n", val);          break;
                }
                fprintf(out, "\tsc.w.aqrl t6, t5, (%s)\n", ptr);
                fprintf(out, "\tbnez t6, %sARM_%u\n", lp, seq);
            } else {
                /* Sub-word (1B/2B) atomic RMW via aligned-word LR/SC loop. */
                uint32_t wmask = (ins->width == 1u) ? 0xFFu : 0xFFFFu;
                fprintf(out, "\tandi t5, %s, -4\n", ptr);
                fprintf(out, "\tandi t6, %s, 3\n", ptr);
                fprintf(out, "\tslli t6, t6, 3\n");
                fprintf(out, "%sARM_%u:\n", lp, seq);
                fprintf(out, "\tlr.w.aqrl %s, (t5)\n", rd);
                /* Extract old lane in rd (shifted down), compute new lane in t4, insert back. */
                fprintf(out, "\tsrl t4, %s, t6\n", rd);
                switch (rop) {
                    case PIR_RMW_ADD:  fprintf(out, "\tadd t4, t4, %s\n", val); break;
                    case PIR_RMW_SUB:  fprintf(out, "\tsub t4, t4, %s\n", val); break;
                    case PIR_RMW_AND:  fprintf(out, "\tand t4, t4, %s\n", val); break;
                    case PIR_RMW_OR:   fprintf(out, "\tor t4, t4, %s\n",  val); break;
                    case PIR_RMW_XOR:  fprintf(out, "\txor t4, t4, %s\n", val); break;
                    case PIR_RMW_XCHG: fprintf(out, "\tmv t4, %s\n", val);      break;
                }
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi t4, t4, 255\n");
                } else {
                    fprintf(out, "\tslli t4, t4, 16\n\tsrli t4, t4, 16\n");
                }
                fprintf(out, "\tsll t4, t4, t6\n");
                /* Clear lane bits in word using stack-free sequence: build mask in sp-safe way via rd?
                 * Wait: we have rd (old word) and t4 (shifted new lane). Use xori/and/or! */
                fprintf(out, "\txor t4, t4, %s\n", rd);
                /* Mask t4 to lane bits only: shift right then left via a temp? Wait:
                 * (t4 >> t6) & wmask << t6! */
                fprintf(out, "\tsrl t4, t4, t6\n");
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi t4, t4, 255\n");
                } else {
                    fprintf(out, "\tslli t4, t4, 16\n\tsrli t4, t4, 16\n");
                }
                fprintf(out, "\tsll t4, t4, t6\n");
                fprintf(out, "\txor t4, t4, %s\n", rd);
                fprintf(out, "\tsc.w.aqrl t4, t4, (t5)\n");
                fprintf(out, "\tbnez t4, %sARM_%u\n", lp, seq);
                fprintf(out, "\tsrl %s, %s, t6\n", rd, rd);
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi %s, %s, 255\n", rd, rd);
                } else {
                    fprintf(out, "\tslli %s, %s, 16\n\tsrli %s, %s, 16\n", rd, rd, rd, rd);
                }
            }
            return;
        }

        case MI_RV32_ATOMIC_CAS: {
            const char *r_old = gp_name(ins->ops[0].u.preg);
            const char *r_ok  = gp_name(ins->ops[1].u.preg);
            const char *ptr   = gp_name(ins->ops[2].u.preg);
            const char *exp   = gp_name(ins->ops[3].u.preg);
            const char *des   = gp_name(ins->ops[4].u.preg);
            uint32_t seq = ctx->local_seq++;
            if (ins->width == 4u) {
                fprintf(out, "%sCAS_%u:\n", lp, seq);
                fprintf(out, "\tlr.w.aqrl %s, (%s)\n", r_old, ptr);
                fprintf(out, "\tbne %s, %s, %sCASF_%u\n", r_old, exp, lp, seq);
                fprintf(out, "\tsc.w.aqrl t5, %s, (%s)\n", des, ptr);
                fprintf(out, "\tbnez t5, %sCAS_%u\n", lp, seq);
                fprintf(out, "\tli %s, 1\n", r_ok);
                fprintf(out, "\tj %sCASD_%u\n", lp, seq);
                fprintf(out, "%sCASF_%u:\n", lp, seq);
                fprintf(out, "\tli %s, 0\n", r_ok);
                fprintf(out, "%sCASD_%u:\n", lp, seq);
            } else {
                uint32_t wmask = (ins->width == 1u) ? 0xFFu : 0xFFFFu;
                fprintf(out, "\tandi t5, %s, -4\n", ptr);
                fprintf(out, "\tandi t6, %s, 3\n", ptr);
                fprintf(out, "\tslli t6, t6, 3\n");
                fprintf(out, "%sCAS_%u:\n", lp, seq);
                fprintf(out, "\tlr.w.aqrl %s, (t5)\n", r_old);
                fprintf(out, "\tsrl t4, %s, t6\n", r_old);
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi t4, t4, 255\n");
                    fprintf(out, "\tandi %s, %s, 255\n", r_ok, exp);
                } else {
                    fprintf(out, "\tslli t4, t4, 16\n\tsrli t4, t4, 16\n");
                    fprintf(out, "\tslli %s, %s, 16\n\tsrli %s, %s, 16\n", r_ok, exp, r_ok, r_ok);
                }
                fprintf(out, "\tbne t4, %s, %sCASF_%u\n", r_ok, lp, seq);
                fprintf(out, "\txor t4, t4, %s\n", des);
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi t4, t4, 255\n");
                } else {
                    fprintf(out, "\tslli t4, t4, 16\n\tsrli t4, t4, 16\n");
                }
                fprintf(out, "\tsll t4, t4, t6\n");
                fprintf(out, "\txor t4, t4, %s\n", r_old);
                fprintf(out, "\tsc.w.aqrl t4, t4, (t5)\n");
                fprintf(out, "\tbnez t4, %sCAS_%u\n", lp, seq);
                fprintf(out, "\tsrl %s, %s, t6\n", r_old, r_old);
                if (wmask == 0xFFu) {
                    fprintf(out, "\tandi %s, %s, 255\n", r_old, r_old);
                } else {
                    fprintf(out, "\tslli %s, %s, 16\n\tsrli %s, %s, 16\n", r_old, r_old, r_old, r_old);
                }
                fprintf(out, "\tli %s, 1\n", r_ok);
                fprintf(out, "\tj %sCASD_%u\n", lp, seq);
                fprintf(out, "%sCASF_%u:\n", lp, seq);
                fprintf(out, "\tmv %s, t4\n", r_old);
                fprintf(out, "\tli %s, 0\n", r_ok);
                fprintf(out, "%sCASD_%u:\n", lp, seq);
            }
            return;
        }

        case MI_RV32_FMV_RR:
            if (ins->ops[0].u.preg != ins->ops[1].u.preg) {
                fprintf(out, "\t%s %s, %s\n", ins->width == 4u ? "fmv.s" : "fmv.d",
                        fp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            }
            return;

        case MI_RV32_FMV_W_X:
            fprintf(out, "\tfmv.w.x %s, %s\n", fp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FMV_X_W:
            fprintf(out, "\tfmv.x.w %s, %s\n", gp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_LOAD_IMM_F64: {
            uint64_t bits = (uint64_t)ins->ops[1].u.imm;
            const char *fd = fp_name(ins->ops[0].u.preg);
            if (bits == 0u) {
                fprintf(out, "\tfcvt.d.w %s, zero\n", fd);
            } else {
                uint32_t cid = add_f64_const(ctx, bits);
                uint32_t seq = ctx->local_seq++;
                fprintf(out, "%sFHI_%u:\n", lp, seq);
                fprintf(out, "\tauipc t5, %%pcrel_hi(%sFC%u_%u)\n", lp, mf->fn_index, cid);
                fprintf(out, "\tfld %s, %%pcrel_lo(%sFHI_%u)(t5)\n", fd, lp, seq);
            }
            return;
        }

        case MI_RV32_FADD: case MI_RV32_FSUB: case MI_RV32_FMUL: case MI_RV32_FDIV: {
            const char *mn = (ins->op == MI_RV32_FADD) ? "fadd"
                           : (ins->op == MI_RV32_FSUB) ? "fsub"
                           : (ins->op == MI_RV32_FMUL) ? "fmul" : "fdiv";
            fprintf(out, "\t%s.%c %s, %s, %s\n", mn, ins->width == 4u ? 's' : 'd',
                    fp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg), fp_name(ins->ops[2].u.preg));
            return;
        }

        case MI_RV32_FNEG: case MI_RV32_FABS: case MI_RV32_FSQRT: {
            const char *mn = (ins->op == MI_RV32_FNEG) ? "fneg"
                           : (ins->op == MI_RV32_FABS) ? "fabs" : "fsqrt";
            fprintf(out, "\t%s.%c %s, %s\n", mn, ins->width == 4u ? 's' : 'd',
                    fp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;
        }

        case MI_RV32_FEQ: case MI_RV32_FLT: case MI_RV32_FLE: {
            const char *mn = (ins->op == MI_RV32_FEQ) ? "feq"
                           : (ins->op == MI_RV32_FLT) ? "flt" : "fle";
            fprintf(out, "\t%s.%c %s, %s, %s\n", mn, ins->width == 4u ? 's' : 'd',
                    gp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg), fp_name(ins->ops[2].u.preg));
            return;
        }

        case MI_RV32_FCVT_S_D:
            fprintf(out, "\tfcvt.s.d %s, %s\n", fp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FCVT_D_S:
            fprintf(out, "\tfcvt.d.s %s, %s\n", fp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FCVT_F_W:
            fprintf(out, "\tfcvt.%c.w %s, %s\n", ins->width == 4u ? 's' : 'd',
                    fp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FCVT_F_WU:
            fprintf(out, "\tfcvt.%c.wu %s, %s\n", ins->width == 4u ? 's' : 'd',
                    fp_name(ins->ops[0].u.preg), gp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FCVT_W_F:
            fprintf(out, "\tfcvt.w.%c %s, %s, rtz\n", ins->width == 4u ? 's' : 'd',
                    gp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_FCVT_WU_F:
            fprintf(out, "\tfcvt.wu.%c %s, %s, rtz\n", ins->width == 4u ? 's' : 'd',
                    gp_name(ins->ops[0].u.preg), fp_name(ins->ops[1].u.preg));
            return;

        case MI_RV32_J:
            fprintf(out, "\tj %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, ins->ops[0].u.block_id));
            return;

        case MI_RV32_BCC:
            emit_bcc(ctx, mf, ins->ops[0].u.cond,
                     gp_name(ins->ops[1].u.preg), gp_name(ins->ops[2].u.preg),
                     redir_blk(ctx, ins->ops[3].u.block_id));
            return;

        case MI_RV32_CALL: {
            const char *cname = ins->ops[0].u.sym.name;
            if (rv32_is_bare_call_sym(cname)) {
                fprintf(out, "\tcall %s\n", cname);
            } else {
                uint32_t seq = ctx->local_seq++;
                fprintf(out, "%sC%u_%u:\n", lp, mf->fn_index, seq);
                fputs("\tauipc ra, %pcrel_hi(", out);
                rv32_emit_sym(out, target, cname);
                fputs(")\n", out);
                fprintf(out, "\tjalr ra, %%pcrel_lo(%sC%u_%u)(ra)\n", lp, mf->fn_index, seq);
            }
            return;
        }

        case MI_RV32_CALL_IND:
            fprintf(out, "\tjalr ra, 0(%s)\n", gp_name(ins->ops[0].u.preg));
            return;

        case MI_RV32_SWITCH_TBL: {
            uint32_t jt_id = ins->ops[1].u.jtable_id;
            const MJTable *jt = &mf->jtables.data[jt_id];
            const char *rv = gp_name(ins->ops[0].u.preg);
            uint32_t def_b = redir_blk(ctx, ins->ops[2].u.block_id);
            if (jt->min_val == 0) {
                fprintf(out, "\tmv t5, %s\n", rv);
            } else if (jt->min_val >= -2047 && jt->min_val <= 2048) {
                fprintf(out, "\taddi t5, %s, %" PRId64 "\n", rv, -jt->min_val);
            } else {
                fprintf(out, "\tli t6, %" PRId64 "\n", jt->min_val);
                fprintf(out, "\tsub t5, %s, t6\n", rv);
            }
            fprintf(out, "\tli t6, %u\n", jt->num_entries);
            emit_bcc(ctx, mf, MCOND_CS, "t5", "t6", def_b);
            fprintf(out, "\tlla t6, %sJT%u_%u\n", lp, mf->fn_index, jt_id);
            fprintf(out, "\tslli t5, t5, 2\n");
            fprintf(out, "\tadd t5, t6, t5\n");
            fprintf(out, "\tlw t5, 0(t5)\n");
            fprintf(out, "\tadd t5, t6, t5\n");
            fprintf(out, "\tjr t5\n");
            return;
        }

        case MI_RV32_RET:
            fputs("\tret\n", out);
            return;

        case MI_RV32_UNIMP:
            fputs("\tunimp\n", out);
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
            fprintf(stderr, "pirc: RV32 asm emitter encountered non-RV32 opcode %u\n", (unsigned)ins->op);
            abort();
    }
}

static bool rv32_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    Rv32AsmCtx *ctx = (Rv32AsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    ctx->f64_pool.len = 0u;

    uint32_t total_insts = 0u;
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        total_insts += mf->blocks.data[b].insts.len;
    }
    ctx->need_branch_relax = (total_insts > 700u);

    uint32_t *blk_redir = NULL;
    bool     *blk_skip  = NULL;
    pir_asm_build_blk_redir(mf, MI_RV32_J, &blk_redir, &blk_skip);
    ctx->blk_redir = blk_redir;

    pir_asm_emit_func_prologue(out, mod, target, mf, 2u, "@", rv32_needs_sym_quote);

    PirDwarfLocState loc_st = { 0u, false };
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        if (b > 0u && blk_skip && blk_skip[b]) continue;
        const MBlock *blk = &mf->blocks.data[b];
        if (b > 0u) {
            fprintf(out, "%sBB%u_%u:\n", lp, mf->fn_index, b);
        }
        uint32_t next_live = b + 1u;
        while (next_live < mf->blocks.len && blk_skip && blk_skip[next_live]) next_live++;

        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            const MInst *ins = &blk->insts.data[j];
            if (ins->op == MI_RV32_J && j + 1u == blk->insts.len &&
                redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            pir_asm_emit_dwarf_loc(out, mod, mf->pir_fn, &loc_st, ins->loc_idx);
            emit_inst(ctx, mf, ins);
        }
    }

    pir_asm_emit_jtables(out, target, mf, blk_redir);
    pir_asm_emit_func_epilogue(out, mod, target, mf, rv32_needs_sym_quote);

    if (ctx->f64_pool.len > 0u) {
        fputs("\t.section .rodata\n", out);
        fputs("\t.p2align 3\n", out);
        for (uint32_t i = 0u; i < ctx->f64_pool.len; ++i) {
            fprintf(out, "%sFC%u_%u:\n", lp, mf->fn_index, i);
            fprintf(out, "\t.quad 0x%" PRIx64 "\n", ctx->f64_pool.data[i]);
        }
        fputc('\n', out);
    }

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);
    return true;
}

static bool rv32_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                              const PirTargetSpec *target) {
    Rv32AsmCtx *ctx = (Rv32AsmCtx *)em->ctx;
    return pir_asm_emit_globals(ctx->out, mod, target, rv32_needs_sym_quote);
}

static bool rv32_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                             const PirTargetSpec *target) {
    Rv32AsmCtx *ctx = (Rv32AsmCtx *)em->ctx;
    PIR_VEC_FREE(&ctx->f64_pool);
    return pir_asm_emit_module_footer(ctx->out, mod, target);
}

void pir_rv32_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target) {
    Rv32AsmCtx *ctx = (Rv32AsmCtx *)calloc(1u, sizeof(Rv32AsmCtx));
    if (!ctx) abort();
    ctx->out    = out;
    ctx->target = target;
    em->ctx                = ctx;
    em->emit_module_header = rv32_emit_header;
    em->emit_function      = rv32_emit_function;
    em->emit_globals       = rv32_emit_globals;
    em->emit_module_footer = rv32_emit_footer;
}
