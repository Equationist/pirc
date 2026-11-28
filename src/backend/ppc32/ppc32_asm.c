/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * PowerPC 32-bit Big-Endian (SysV SVR4) GAS/LLVM-MC Assembly Emitter (ELF).
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
    PIR_VEC(uint32_t)    f32_pool;
    PIR_VEC(uint64_t)    f64_pool;
} PpcAsmCtx;

static uint32_t redir_blk(const PpcAsmCtx *ctx, uint32_t bid) {
    return ctx->blk_redir ? ctx->blk_redir[bid] : bid;
}

static unsigned gp_num(MPReg p) {
    return (unsigned)(ppc_reg_num(p) & 31u);
}

static unsigned fp_num(MPReg p) {
    return (unsigned)(ppc_reg_num(p) & 31u);
}

static bool ppc32_needs_sym_quote(const char *s) {
    /* PowerPC ELF uses bare numeric registers (0..31), so register-like names
     * (r0..r31, f0..f31, cr0..cr7, sp, lr, ctr, xer) are valid bare symbols. */
    (void)s;
    return false;
}

static bool ppc32_sym_requires_alias(const char *s) {
    if (!s || !s[0]) return true;
    unsigned char c0 = (unsigned char)s[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_')) {
        return true;
    }
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return true;
        }
    }
    return false;
}

static void ppc32_emit_sym(FILE *out, const PirTargetSpec *target, const char *name) {
    pir_asm_emit_sym(out, target, name, NULL, ppc32_needs_sym_quote);
}

/* LLVM-MC's PowerPC parser rejects quoted string symbols inside instruction
 * operands (lis @ha / addi @l / addis @tprel@ha / bl). For any non-bare symbol
 * name, emit a local .set alias right before the instruction and reference it. */
static void ppc32_prepare_op_sym(PpcAsmCtx *ctx, const char *name, char *alias_buf, size_t alias_sz) {
    if (!ppc32_sym_requires_alias(name)) {
        alias_buf[0] = '\0';
        return;
    }
    uint32_t seq = ctx->local_seq++;
    const char *lp = pir_asm_lbl_pfx(ctx->target);
    snprintf(alias_buf, alias_sz, "%sPS%u", lp, seq);
    fprintf(ctx->out, "\t.set %s, ", alias_buf);
    ppc32_emit_sym(ctx->out, ctx->target, name);
    fputc('\n', ctx->out);
}

static uint32_t add_f32_const(PpcAsmCtx *ctx, uint32_t bits) {
    for (uint32_t i = 0u; i < ctx->f32_pool.len; ++i) {
        if (ctx->f32_pool.data[i] == bits) return i;
    }
    uint32_t id = ctx->f32_pool.len;
    PIR_VEC_PUSH(&ctx->f32_pool, bits);
    return id;
}

static uint32_t add_f64_const(PpcAsmCtx *ctx, uint64_t bits) {
    for (uint32_t i = 0u; i < ctx->f64_pool.len; ++i) {
        if (ctx->f64_pool.data[i] == bits) return i;
    }
    uint32_t id = ctx->f64_pool.len;
    PIR_VEC_PUSH(&ctx->f64_pool, bits);
    return id;
}

static void emit_li_imm(FILE *out, unsigned rd, int32_t v) {
    if (v >= -32768 && v <= 32767) {
        fprintf(out, "\tli %u, %d\n", rd, (int)v);
    } else {
        uint32_t u = (uint32_t)v;
        int16_t hi = (int16_t)(u >> 16u);
        uint32_t lo = u & 0xFFFFu;
        fprintf(out, "\tlis %u, %d\n", rd, (int)hi);
        if (lo != 0u) {
            fprintf(out, "\tori %u, %u, %u\n", rd, rd, (unsigned)lo);
        }
    }
}

static void emit_bc(PpcAsmCtx *ctx, const MFunc *mf, MCond c, uint32_t dst_blk) {
    FILE *out = ctx->out;
    const char *lp = pir_asm_lbl_pfx(ctx->target);
    const char *mn = "beq";
    const char *inv = "bne";

    switch (c) {
        case MCOND_EQ: mn = "beq"; inv = "bne"; break;
        case MCOND_NE: mn = "bne"; inv = "beq"; break;
        case MCOND_LT: case MCOND_CC: case MCOND_MI: mn = "blt"; inv = "bge"; break;
        case MCOND_GE: case MCOND_CS: case MCOND_PL: mn = "bge"; inv = "blt"; break;
        case MCOND_GT: case MCOND_HI: mn = "bgt"; inv = "ble"; break;
        case MCOND_LE: case MCOND_LS: mn = "ble"; inv = "bgt"; break;
        case MCOND_VS: case MCOND_P:  mn = "bun"; inv = "bnu"; break;
        case MCOND_VC: case MCOND_NP: mn = "bnu"; inv = "bun"; break;
    }
    if (ctx->need_branch_relax) {
        uint32_t seq = ctx->local_seq++;
        fprintf(out, "\t%s %sBRX%u\n", inv, lp, seq);
        fprintf(out, "\tb %sBB%u_%u\n", lp, mf->fn_index, dst_blk);
        fprintf(out, "%sBRX%u:\n", lp, seq);
    } else {
        fprintf(out, "\t%s %sBB%u_%u\n", mn, lp, mf->fn_index, dst_blk);
    }
}

static bool ppc32_emit_header(PirCodeEmitter *em, const PirModule *mod,
                              const PirTargetSpec *target) {
    PpcAsmCtx *ctx = (PpcAsmCtx *)em->ctx;
    pir_asm_emit_section_directive(ctx->out, mod, target, NULL, PIR_GSEG_RODATA, false, false, true);
    pir_asm_emit_dwarf_files(ctx->out, mod);
    return true;
}

static void emit_inst(PpcAsmCtx *ctx, const MFunc *mf, const MInst *ins) {
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    switch (ins->op) {
        case MI_NOP:
            return;

        case MI_PPC_LI:
            emit_li_imm(out, gp_num(ins->ops[0].u.preg), (int32_t)ins->ops[1].u.imm);
            return;

        case MI_PPC_MR:
            if (ins->ops[0].u.preg != ins->ops[1].u.preg) {
                fprintf(out, "\tmr %u, %u\n", gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg));
            }
            return;

        case MI_PPC_MOV_SP:
            if (ins->aux_id == 0u) {
                fprintf(out, "\tmr %u, 1\n", gp_num(ins->ops[0].u.preg));
            } else {
                fprintf(out, "\tmr 1, %u\n", gp_num(ins->ops[0].u.preg));
            }
            return;

        case MI_PPC_MFLR:
            fprintf(out, "\tmflr %u\n", gp_num(ins->ops[0].u.preg));
            return;

        case MI_PPC_MTLR:
            fprintf(out, "\tmtlr %u\n", gp_num(ins->ops[0].u.preg));
            return;

        case MI_PPC_STWU:
            fprintf(out, "\tstwu 1, %" PRId64 "(1)\n", ins->ops[0].u.imm);
            return;

        case MI_PPC_SYM_ADDR: {
            unsigned rd = gp_num(ins->ops[0].u.preg);
            char abuf[64];
            ppc32_prepare_op_sym(ctx, ins->ops[1].u.sym.name, abuf, sizeof(abuf));
            fprintf(out, "\tlis %u, ", rd);
            if (abuf[0]) fputs(abuf, out);
            else ppc32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs("@ha\n", out);
            fprintf(out, "\taddi %u, %u, ", rd, rd);
            if (abuf[0]) fputs(abuf, out);
            else ppc32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs("@l\n", out);
            return;
        }

        case MI_PPC_TLS_ADDR: {
            unsigned rd = gp_num(ins->ops[0].u.preg);
            char abuf[64];
            ppc32_prepare_op_sym(ctx, ins->ops[1].u.sym.name, abuf, sizeof(abuf));
            fprintf(out, "\taddis %u, 2, ", rd);
            if (abuf[0]) fputs(abuf, out);
            else ppc32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs("@tprel@ha\n", out);
            fprintf(out, "\taddi %u, %u, ", rd, rd);
            if (abuf[0]) fputs(abuf, out);
            else ppc32_emit_sym(out, target, ins->ops[1].u.sym.name);
            fputs("@tprel@l\n", out);
            return;
        }

        case MI_PPC_LD: {
            unsigned rd = gp_num(ins->ops[0].u.preg);
            unsigned rb = gp_num(ins->ops[1].u.preg);
            int64_t  d  = ins->ops[2].u.imm;
            if (ins->width == 1u) {
                fprintf(out, "\tlbz %u, %" PRId64 "(%u)\n", rd, d, rb);
                if (ins->is_signed) fprintf(out, "\textsb %u, %u\n", rd, rd);
            } else if (ins->width == 2u) {
                fprintf(out, "\t%s %u, %" PRId64 "(%u)\n", ins->is_signed ? "lha" : "lhz", rd, d, rb);
            } else {
                fprintf(out, "\tlwz %u, %" PRId64 "(%u)\n", rd, d, rb);
            }
            return;
        }

        case MI_PPC_ST: {
            const char *mn = (ins->width == 1u) ? "stb" : (ins->width == 2u) ? "sth" : "stw";
            fprintf(out, "\t%s %u, %" PRId64 "(%u)\n", mn,
                    gp_num(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_num(ins->ops[1].u.preg));
            return;
        }

        case MI_PPC_FLD:
            fprintf(out, "\t%s %u, %" PRId64 "(%u)\n", ins->width == 4u ? "lfs" : "lfd",
                    fp_num(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FST:
            fprintf(out, "\t%s %u, %" PRId64 "(%u)\n", ins->width == 4u ? "stfs" : "stfd",
                    fp_num(ins->ops[0].u.preg), ins->ops[2].u.imm, gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_SUB:
            /* subf rD, rA, rB computes rB - rA; ops[1]=lhs, ops[2]=rhs -> lhs - rhs */
            fprintf(out, "\tsubf %u, %u, %u\n",
                    gp_num(ins->ops[0].u.preg), gp_num(ins->ops[2].u.preg), gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_ADD:   case MI_PPC_MULLW: case MI_PPC_MULHW: case MI_PPC_MULHWU:
        case MI_PPC_DIVW:  case MI_PPC_DIVWU:
        case MI_PPC_AND:   case MI_PPC_OR:    case MI_PPC_XOR:
        case MI_PPC_SLW:   case MI_PPC_SRW:   case MI_PPC_SRAW: {
            const char *mn = (ins->op == MI_PPC_ADD)    ? "add"
                           : (ins->op == MI_PPC_MULLW)  ? "mullw"
                           : (ins->op == MI_PPC_MULHW)  ? "mulhw"
                           : (ins->op == MI_PPC_MULHWU) ? "mulhwu"
                           : (ins->op == MI_PPC_DIVW)   ? "divw"
                           : (ins->op == MI_PPC_DIVWU)  ? "divwu"
                           : (ins->op == MI_PPC_AND)    ? "and"
                           : (ins->op == MI_PPC_OR)     ? "or"
                           : (ins->op == MI_PPC_XOR)    ? "xor"
                           : (ins->op == MI_PPC_SLW)    ? "slw"
                           : (ins->op == MI_PPC_SRW)    ? "srw" : "sraw";
            fprintf(out, "\t%s %u, %u, %u\n", mn,
                    gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg), gp_num(ins->ops[2].u.preg));
            return;
        }

        case MI_PPC_ADDI: case MI_PPC_ANDI: case MI_PPC_ORI: case MI_PPC_XORI:
        case MI_PPC_SLWI: case MI_PPC_SRWI: case MI_PPC_SRAWI: {
            const char *mn = (ins->op == MI_PPC_ADDI)  ? "addi"
                           : (ins->op == MI_PPC_ANDI)  ? "andi."
                           : (ins->op == MI_PPC_ORI)   ? "ori"
                           : (ins->op == MI_PPC_XORI)  ? "xori"
                           : (ins->op == MI_PPC_SLWI)  ? "slwi"
                           : (ins->op == MI_PPC_SRWI)  ? "srwi" : "srawi";
            fprintf(out, "\t%s %u, %u, %" PRId64 "\n", mn,
                    gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg), ins->ops[2].u.imm);
            return;
        }

        case MI_PPC_EXTSB:
            fprintf(out, "\textsb %u, %u\n", gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_EXTSH:
            fprintf(out, "\textsh %u, %u\n", gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_CMPW:
            fprintf(out, "\tcmpw 0, %u, %u\n", gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_CMPWI:
            fprintf(out, "\tcmpwi 0, %u, %" PRId64 "\n", gp_num(ins->ops[0].u.preg), ins->ops[1].u.imm);
            return;

        case MI_PPC_CMPLW:
            fprintf(out, "\tcmplw 0, %u, %u\n", gp_num(ins->ops[0].u.preg), gp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_CMPLWI:
            fprintf(out, "\tcmplwi 0, %u, %" PRId64 "\n", gp_num(ins->ops[0].u.preg), ins->ops[1].u.imm);
            return;

        case MI_PPC_SETCC: {
            unsigned rd = gp_num(ins->ops[0].u.preg);
            MCond c = ins->ops[1].u.cond;
            unsigned rot = 3u;
            bool inv = false;
            switch (c) {
                case MCOND_LT: case MCOND_CC: case MCOND_MI: rot = 1u; inv = false; break;
                case MCOND_GE: case MCOND_CS: case MCOND_PL: rot = 1u; inv = true;  break;
                case MCOND_GT: case MCOND_HI: rot = 2u; inv = false; break;
                case MCOND_LE: case MCOND_LS: rot = 2u; inv = true;  break;
                case MCOND_EQ:                rot = 3u; inv = false; break;
                case MCOND_NE:                rot = 3u; inv = true;  break;
                case MCOND_VS: case MCOND_P:  rot = 4u; inv = false; break;
                case MCOND_VC: case MCOND_NP: rot = 4u; inv = true;  break;
            }
            fprintf(out, "\tmfcr %u\n", rd);
            fprintf(out, "\trlwinm %u, %u, %u, 31, 31\n", rd, rd, rot);
            if (inv) fprintf(out, "\txori %u, %u, 1\n", rd, rd);
            return;
        }

        case MI_PPC_SYNC:
            switch ((PirMemOrder)ins->aux_id) {
                case PIR_ORDER_NONE:
                case PIR_ORDER_RELAXED:
                    break;
                case PIR_ORDER_ACQUIRE:
                    fputs("\tlwsync\n", out);
                    break;
                case PIR_ORDER_RELEASE:
                case PIR_ORDER_ACQ_REL:
                case PIR_ORDER_SEQ_CST:
                    fputs("\tsync\n", out);
                    break;
            }
            return;

        case MI_PPC_ATOMIC_RMW: {
            unsigned rd  = gp_num(ins->ops[0].u.preg);
            unsigned ptr = gp_num(ins->ops[1].u.preg);
            unsigned val = gp_num(ins->ops[2].u.preg);
            uint32_t seq = ctx->local_seq++;
            PirRmwOp rop = (PirRmwOp)ins->aux_id;

            fputs("\tsync\n", out);
            if (ins->width == 4u) {
                fprintf(out, "%sARM_%u:\n", lp, seq);
                fprintf(out, "\tlwarx %u, 0, %u\n", rd, ptr);
                switch (rop) {
                    case PIR_RMW_ADD:  fprintf(out, "\tadd 11, %u, %u\n", rd, val);  break;
                    case PIR_RMW_SUB:  fprintf(out, "\tsubf 11, %u, %u\n", val, rd); break;
                    case PIR_RMW_AND:  fprintf(out, "\tand 11, %u, %u\n", rd, val);  break;
                    case PIR_RMW_OR:   fprintf(out, "\tor 11, %u, %u\n", rd, val);   break;
                    case PIR_RMW_XOR:  fprintf(out, "\txor 11, %u, %u\n", rd, val);  break;
                    case PIR_RMW_XCHG: fprintf(out, "\tmr 11, %u\n", val);           break;
                }
                fprintf(out, "\tstwcx. 11, 0, %u\n", ptr);
                fprintf(out, "\tbne- %sARM_%u\n", lp, seq);
                fputs("\tisync\n", out);
            } else {
                /* Big-endian sub-word (1B/2B) atomic RMW via aligned-word lwarx/stwcx.
                 * Byte offset b = ptr & 3 -> BE bit shift = ((ptr & 3) ^ 3) * 8.
                 * Half offset b = ptr & 2 -> BE bit shift = ((ptr & 2) ^ 2) * 8. */
                unsigned wmask   = (ins->width == 1u) ? 255u : 65535u;
                unsigned offmask = (ins->width == 1u) ? 3u : 2u;
                fprintf(out, "\trlwinm 11, %u, 0, 0, 29\n", ptr);
                fprintf(out, "\tandi. 12, %u, %u\n", ptr, offmask);
                fprintf(out, "\txori 12, 12, %u\n", offmask);
                fprintf(out, "\tslwi 12, 12, 3\n");
                fprintf(out, "%sARM_%u:\n", lp, seq);
                fprintf(out, "\tlwarx %u, 0, 11\n", rd);
                fprintf(out, "\tsrw 0, %u, 12\n", rd);
                switch (rop) {
                    case PIR_RMW_ADD:  fprintf(out, "\tadd 0, 0, %u\n", val);  break;
                    case PIR_RMW_SUB:  fprintf(out, "\tsubf 0, %u, 0\n", val); break;
                    case PIR_RMW_AND:  fprintf(out, "\tand 0, 0, %u\n", val);  break;
                    case PIR_RMW_OR:   fprintf(out, "\tor 0, 0, %u\n", val);   break;
                    case PIR_RMW_XOR:  fprintf(out, "\txor 0, 0, %u\n", val);  break;
                    case PIR_RMW_XCHG: fprintf(out, "\tmr 0, %u\n", val);      break;
                }
                fprintf(out, "\tandi. 0, 0, %u\n", wmask);
                fprintf(out, "\tslw 0, 0, 12\n");
                fprintf(out, "\txor 0, 0, %u\n", rd);
                fprintf(out, "\tsrw 0, 0, 12\n");
                fprintf(out, "\tandi. 0, 0, %u\n", wmask);
                fprintf(out, "\tslw 0, 0, 12\n");
                fprintf(out, "\txor 0, 0, %u\n", rd);
                fprintf(out, "\tstwcx. 0, 0, 11\n");
                fprintf(out, "\tbne- %sARM_%u\n", lp, seq);
                fputs("\tisync\n", out);
                fprintf(out, "\tsrw %u, %u, 12\n", rd, rd);
                fprintf(out, "\tandi. %u, %u, %u\n", rd, rd, wmask);
            }
            return;
        }

        case MI_PPC_ATOMIC_CAS: {
            unsigned r_old = gp_num(ins->ops[0].u.preg);
            unsigned r_ok  = gp_num(ins->ops[1].u.preg);
            unsigned ptr   = gp_num(ins->ops[2].u.preg);
            unsigned exp   = gp_num(ins->ops[3].u.preg);
            unsigned des   = gp_num(ins->ops[4].u.preg);
            uint32_t seq = ctx->local_seq++;

            fputs("\tsync\n", out);
            if (ins->width == 4u) {
                fprintf(out, "%sCAS_%u:\n", lp, seq);
                fprintf(out, "\tlwarx %u, 0, %u\n", r_old, ptr);
                fprintf(out, "\tcmpw 0, %u, %u\n", r_old, exp);
                fprintf(out, "\tbne- %sCASF_%u\n", lp, seq);
                fprintf(out, "\tstwcx. %u, 0, %u\n", des, ptr);
                fprintf(out, "\tbne- %sCAS_%u\n", lp, seq);
                fputs("\tisync\n", out);
                fprintf(out, "\tli %u, 1\n", r_ok);
                fprintf(out, "\tb %sCASD_%u\n", lp, seq);
                fprintf(out, "%sCASF_%u:\n", lp, seq);
                fprintf(out, "\tli %u, 0\n", r_ok);
                fprintf(out, "%sCASD_%u:\n", lp, seq);
            } else {
                unsigned wmask   = (ins->width == 1u) ? 255u : 65535u;
                unsigned offmask = (ins->width == 1u) ? 3u : 2u;
                fprintf(out, "\trlwinm 11, %u, 0, 0, 29\n", ptr);
                fprintf(out, "\tandi. 12, %u, %u\n", ptr, offmask);
                fprintf(out, "\txori 12, 12, %u\n", offmask);
                fprintf(out, "\tslwi 12, 12, 3\n");
                fprintf(out, "%sCAS_%u:\n", lp, seq);
                fprintf(out, "\tlwarx %u, 0, 11\n", r_old);
                fprintf(out, "\tsrw 0, %u, 12\n", r_old);
                fprintf(out, "\tandi. 0, 0, %u\n", wmask);
                fprintf(out, "\tandi. %u, %u, %u\n", r_ok, exp, wmask);
                fprintf(out, "\tcmpw 0, 0, %u\n", r_ok);
                fprintf(out, "\tbne- %sCASF_%u\n", lp, seq);
                fprintf(out, "\txor 0, 0, %u\n", des);
                fprintf(out, "\tandi. 0, 0, %u\n", wmask);
                fprintf(out, "\tslw 0, 0, 12\n");
                fprintf(out, "\txor 0, 0, %u\n", r_old);
                fprintf(out, "\tstwcx. 0, 0, 11\n");
                fprintf(out, "\tbne- %sCAS_%u\n", lp, seq);
                fputs("\tisync\n", out);
                fprintf(out, "\tsrw %u, %u, 12\n", r_old, r_old);
                fprintf(out, "\tandi. %u, %u, %u\n", r_old, r_old, wmask);
                fprintf(out, "\tli %u, 1\n", r_ok);
                fprintf(out, "\tb %sCASD_%u\n", lp, seq);
                fprintf(out, "%sCASF_%u:\n", lp, seq);
                fprintf(out, "\tmr %u, 0\n", r_old);
                fprintf(out, "\tli %u, 0\n", r_ok);
                fprintf(out, "%sCASD_%u:\n", lp, seq);
            }
            return;
        }

        case MI_PPC_FMR:
            if (ins->ops[0].u.preg != ins->ops[1].u.preg) {
                fprintf(out, "\tfmr %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            }
            return;

        case MI_PPC_LOAD_IMM_FP: {
            unsigned fd = fp_num(ins->ops[0].u.preg);
            uint64_t bits = (uint64_t)ins->ops[1].u.imm;
            if (ins->width == 4u) {
                uint32_t cid = add_f32_const(ctx, (uint32_t)bits);
                fprintf(out, "\tlis 12, %sFS%u_%u@ha\n", lp, mf->fn_index, cid);
                fprintf(out, "\tlfs %u, %sFS%u_%u@l(12)\n", fd, lp, mf->fn_index, cid);
            } else {
                uint32_t cid = add_f64_const(ctx, bits);
                fprintf(out, "\tlis 12, %sFD%u_%u@ha\n", lp, mf->fn_index, cid);
                fprintf(out, "\tlfd %u, %sFD%u_%u@l(12)\n", fd, lp, mf->fn_index, cid);
            }
            return;
        }

        case MI_PPC_FADD: case MI_PPC_FSUB: case MI_PPC_FMUL: case MI_PPC_FDIV: {
            const char *mn = (ins->op == MI_PPC_FADD) ? (ins->width == 4u ? "fadds" : "fadd")
                           : (ins->op == MI_PPC_FSUB) ? (ins->width == 4u ? "fsubs" : "fsub")
                           : (ins->op == MI_PPC_FMUL) ? (ins->width == 4u ? "fmuls" : "fmul")
                                                      : (ins->width == 4u ? "fdivs" : "fdiv");
            fprintf(out, "\t%s %u, %u, %u\n", mn,
                    fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg), fp_num(ins->ops[2].u.preg));
            return;
        }

        case MI_PPC_FNEG:
            fprintf(out, "\tfneg %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FABS:
            fprintf(out, "\tfabs %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FSQRT:
            fprintf(out, "\t%s %u, %u\n", ins->width == 4u ? "fsqrts" : "fsqrt",
                    fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FRSP:
            fprintf(out, "\tfrsp %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FCMPU:
            fprintf(out, "\tfcmpu 0, %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_FCTIWZ:
            fprintf(out, "\tfctiwz %u, %u\n", fp_num(ins->ops[0].u.preg), fp_num(ins->ops[1].u.preg));
            return;

        case MI_PPC_B:
            fprintf(out, "\tb %sBB%u_%u\n", lp, mf->fn_index, redir_blk(ctx, ins->ops[0].u.block_id));
            return;

        case MI_PPC_BC:
            emit_bc(ctx, mf, ins->ops[0].u.cond, redir_blk(ctx, ins->ops[1].u.block_id));
            return;

        case MI_PPC_CALL: {
            char abuf[64];
            ppc32_prepare_op_sym(ctx, ins->ops[0].u.sym.name, abuf, sizeof(abuf));
            if (ins->aux_id == 1u) fputs("\tcrxor 6, 6, 6\n", out);
            else if (ins->aux_id >= 2u) fputs("\tcreqv 6, 6, 6\n", out);
            fputs("\tbl ", out);
            if (abuf[0]) fputs(abuf, out);
            else ppc32_emit_sym(out, target, ins->ops[0].u.sym.name);
            fputc('\n', out);
            return;
        }

        case MI_PPC_CALL_IND:
            fprintf(out, "\tmtctr %u\n", gp_num(ins->ops[0].u.preg));
            if (ins->aux_id == 1u) fputs("\tcrxor 6, 6, 6\n", out);
            else if (ins->aux_id >= 2u) fputs("\tcreqv 6, 6, 6\n", out);
            fputs("\tbctrl\n", out);
            return;

        case MI_PPC_SWITCH_TBL: {
            uint32_t jt_id = ins->ops[1].u.jtable_id;
            const MJTable *jt = &mf->jtables.data[jt_id];
            unsigned rv = gp_num(ins->ops[0].u.preg);
            uint32_t def_b = redir_blk(ctx, ins->ops[2].u.block_id);
            if (jt->min_val == 0) {
                fprintf(out, "\tmr 11, %u\n", rv);
            } else if (jt->min_val >= -32767 && jt->min_val <= 32768) {
                fprintf(out, "\taddi 11, %u, %" PRId64 "\n", rv, -jt->min_val);
            } else {
                emit_li_imm(out, 12u, (int32_t)jt->min_val);
                fprintf(out, "\tsubf 11, 12, %u\n", rv);
            }
            if (jt->num_entries <= 65535u) {
                fprintf(out, "\tcmplwi 0, 11, %u\n", jt->num_entries);
            } else {
                emit_li_imm(out, 12u, (int32_t)jt->num_entries);
                fputs("\tcmplw 0, 11, 12\n", out);
            }
            emit_bc(ctx, mf, MCOND_GE, def_b);
            fprintf(out, "\tlis 12, %sJT%u_%u@ha\n", lp, mf->fn_index, jt_id);
            fprintf(out, "\taddi 12, 12, %sJT%u_%u@l\n", lp, mf->fn_index, jt_id);
            fputs("\tslwi 11, 11, 2\n", out);
            fputs("\tlwzx 11, 12, 11\n", out);
            fputs("\tadd 11, 12, 11\n", out);
            fputs("\tmtctr 11\n", out);
            fputs("\tbctr\n", out);
            return;
        }

        case MI_PPC_RET:
            fputs("\tblr\n", out);
            return;

        case MI_PPC_TRAP:
            fputs("\ttw 31, 0, 0\n", out);
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
            fprintf(stderr, "pirc: PPC32 asm emitter encountered non-PPC32 opcode %u\n", (unsigned)ins->op);
            abort();
    }
}

static uint32_t ppc32_max_inst_words(const MInst *ins) {
    if (ins->op == MI_PPC_ATOMIC_CAS) return 26u;
    if (ins->op == MI_PPC_ATOMIC_RMW) return 24u;
    if (ins->op == MI_PPC_SWITCH_TBL) return 14u;
    if (ins->op == MI_PPC_SETCC || ins->op == MI_PPC_CALL_IND) return 3u;
    if (ins->op == MI_PPC_LI || ins->op == MI_PPC_SYM_ADDR ||
        ins->op == MI_PPC_TLS_ADDR || ins->op == MI_PPC_LOAD_IMM_FP ||
        ins->op == MI_PPC_LD || ins->op == MI_PPC_CALL || ins->op == MI_PPC_BC) {
        return 2u;
    }
    return 1u;
}

static bool ppc32_emit_function(PirCodeEmitter *em, const PirModule *mod, const MFunc *mf) {
    PpcAsmCtx *ctx = (PpcAsmCtx *)em->ctx;
    FILE *out = ctx->out;
    const PirTargetSpec *target = ctx->target;
    const char *lp = pir_asm_lbl_pfx(target);

    ctx->f32_pool.len = 0u;
    ctx->f64_pool.len = 0u;

    uint32_t total_words = 0u;
    for (uint32_t b = 0u; b < mf->blocks.len; ++b) {
        const MBlock *blk = &mf->blocks.data[b];
        for (uint32_t j = 0u; j < blk->insts.len; ++j) {
            total_words += ppc32_max_inst_words(&blk->insts.data[j]);
        }
    }
    /* PowerPC conditional branch displacement is +-32 KiB (+-8192 words); relax above 6000. */
    ctx->need_branch_relax = (total_words > 6000u);

    uint32_t *blk_redir = NULL;
    bool     *blk_skip  = NULL;
    pir_asm_build_blk_redir(mf, MI_PPC_B, &blk_redir, &blk_skip);
    ctx->blk_redir = blk_redir;

    pir_asm_emit_func_prologue(out, mod, target, mf, 2u, "@", ppc32_needs_sym_quote);

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
            if (ins->op == MI_PPC_B && j + 1u == blk->insts.len &&
                redir_blk(ctx, ins->ops[0].u.block_id) == next_live) {
                continue;
            }
            pir_asm_emit_dwarf_loc(out, mod, mf->pir_fn, &loc_st, ins->loc_idx);
            emit_inst(ctx, mf, ins);
        }
    }

    pir_asm_emit_jtables(out, target, mf, blk_redir);
    pir_asm_emit_func_epilogue(out, mod, target, mf, ppc32_needs_sym_quote);

    if (ctx->f32_pool.len > 0u) {
        fputs("\t.section .rodata\n", out);
        fputs("\t.p2align 2\n", out);
        for (uint32_t i = 0u; i < ctx->f32_pool.len; ++i) {
            fprintf(out, "%sFS%u_%u:\n", lp, mf->fn_index, i);
            fprintf(out, "\t.long 0x%" PRIx32 "\n", ctx->f32_pool.data[i]);
        }
        fputc('\n', out);
    }
    if (ctx->f64_pool.len > 0u) {
        fputs("\t.section .rodata\n", out);
        fputs("\t.p2align 3\n", out);
        for (uint32_t i = 0u; i < ctx->f64_pool.len; ++i) {
            fprintf(out, "%sFD%u_%u:\n", lp, mf->fn_index, i);
            fprintf(out, "\t.quad 0x%" PRIx64 "\n", ctx->f64_pool.data[i]);
        }
        fputc('\n', out);
    }

    ctx->blk_redir = NULL;
    free(blk_redir);
    free(blk_skip);
    return true;
}

static bool ppc32_emit_globals(PirCodeEmitter *em, const PirModule *mod,
                               const PirTargetSpec *target) {
    PpcAsmCtx *ctx = (PpcAsmCtx *)em->ctx;
    return pir_asm_emit_globals(ctx->out, mod, target, ppc32_needs_sym_quote);
}

static bool ppc32_emit_footer(PirCodeEmitter *em, const PirModule *mod,
                              const PirTargetSpec *target) {
    PpcAsmCtx *ctx = (PpcAsmCtx *)em->ctx;
    PIR_VEC_FREE(&ctx->f32_pool);
    PIR_VEC_FREE(&ctx->f64_pool);
    return pir_asm_emit_module_footer(ctx->out, mod, target);
}

void pir_ppc32_asm_emitter_init(PirCodeEmitter *em, FILE *out, const PirTargetSpec *target) {
    PpcAsmCtx *ctx = (PpcAsmCtx *)calloc(1u, sizeof(PpcAsmCtx));
    if (!ctx) abort();
    ctx->out    = out;
    ctx->target = target;
    em->ctx                = ctx;
    em->emit_module_header = ppc32_emit_header;
    em->emit_function      = ppc32_emit_function;
    em->emit_globals       = ppc32_emit_globals;
    em->emit_module_footer = ppc32_emit_footer;
}
