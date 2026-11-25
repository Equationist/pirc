/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_BACKEND_MIR_H
#define PIR_BACKEND_MIR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/arena.h"
#include "common/vec.h"
#include "ir/ir.h"

/* =========================================================================
 * 1. Register Model (Shared across ARM64 & future x86-64 backends)
 * ========================================================================= */

typedef uint32_t MVReg;
typedef uint16_t MPReg;

#define MREG_NONE   ((MPReg)0xFFFFu)
#define MVREG_NONE  ((MVReg)0xFFFFFFFFu)
#define MBLOCK_NONE ((uint32_t)0xFFFFFFFFu)

typedef enum {
    MREG_CLASS_GP = 0,  /* General-purpose integer/pointer registers */
    MREG_CLASS_FP       /* Floating-point & SIMD vector registers */
} MRegClass;

/* ARM64 physical register numbering:
 *   0..30  : x0..x30  (w0..w30)
 *   31     : sp / xzr (special)
 *   32..63 : v0..v31  (s0..s31 / d0..d31 / q0..q31)
 */
#define A64_GP(n)   ((MPReg)(n))
#define A64_FP(n)   ((MPReg)(32u + (n)))
#define A64_SP      ((MPReg)31u)
#define A64_XZR     ((MPReg)31u)
#define A64_FP_REG  ((MPReg)29u)
#define A64_LR_REG  ((MPReg)30u)

/* Dedicated scratch registers (never allocated by regalloc) */
#define A64_SCRATCH_GP0 A64_GP(15u)
#define A64_SCRATCH_GP1 A64_GP(16u)
#define A64_SCRATCH_GP2 A64_GP(17u)
#define A64_SCRATCH_FP0 A64_FP(29u)
#define A64_SCRATCH_FP1 A64_FP(30u)
#define A64_SCRATCH_FP2 A64_FP(31u)

static inline bool a64_is_fp_preg(MPReg r) {
    return r >= 32u && r < 64u;
}

static inline uint32_t a64_reg_num(MPReg r) {
    return (r >= 32u) ? (uint32_t)(r - 32u) : (uint32_t)r;
}

/* x86-64 physical register numbering:
 *   0..15  : rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15
 *   32..47 : xmm0..xmm15
 */
#define X64_GP(n)   ((MPReg)(n))
#define X64_XMM(n)  ((MPReg)(32u + (n)))
#define X64_RAX     X64_GP(0u)
#define X64_RCX     X64_GP(1u)
#define X64_RDX     X64_GP(2u)
#define X64_RBX     X64_GP(3u)
#define X64_RSP     X64_GP(4u)
#define X64_RBP     X64_GP(5u)
#define X64_RSI     X64_GP(6u)
#define X64_RDI     X64_GP(7u)
#define X64_R8      X64_GP(8u)
#define X64_R9      X64_GP(9u)
#define X64_R10     X64_GP(10u)
#define X64_R11     X64_GP(11u)
#define X64_R12     X64_GP(12u)
#define X64_R13     X64_GP(13u)
#define X64_R14     X64_GP(14u)
#define X64_R15     X64_GP(15u)

/* Dedicated x86-64 scratch registers (never allocated by regalloc).
 * r10, r11 are caller-saved; r15 is callee-saved (saved only if touched by 3-way spill). */
#define X64_SCRATCH_GP0 X64_R10
#define X64_SCRATCH_GP1 X64_R11
#define X64_SCRATCH_GP2 X64_R15
#define X64_SCRATCH_FP0 X64_XMM(13u)
#define X64_SCRATCH_FP1 X64_XMM(14u)
#define X64_SCRATCH_FP2 X64_XMM(15u)

static inline bool x64_is_fp_preg(MPReg r) {
    return r >= 32u && r < 48u;
}

static inline uint32_t x64_reg_num(MPReg r) {
    return (r >= 32u) ? (uint32_t)(r - 32u) : (uint32_t)r;
}

/* =========================================================================
 * 2. Condition Codes, NEON Arrangements & Machine Opcodes
 * ========================================================================= */

typedef enum {
    MCOND_EQ = 0,
    MCOND_NE,
    MCOND_CS, /* HS: unsigned >= / carry set */
    MCOND_CC, /* LO: unsigned <  / carry clear */
    MCOND_MI, /* negative / ordered float < */
    MCOND_PL, /* non-negative / unordered-or->= */
    MCOND_VS, /* overflow set / unordered */
    MCOND_VC, /* overflow clear / ordered */
    MCOND_HI, /* unsigned > */
    MCOND_LS, /* unsigned <= */
    MCOND_GE, /* signed >= */
    MCOND_LT, /* signed < */
    MCOND_GT, /* signed > */
    MCOND_LE, /* signed <= */
    MCOND_P,  /* x86-64 parity even (PF=1 / unordered float) */
    MCOND_NP  /* x86-64 parity odd  (PF=0 / ordered float) */
} MCond;

typedef enum {
    MARR_NONE = 0,
    MARR_8B,
    MARR_4H,
    MARR_2S,
    MARR_16B,
    MARR_8H,
    MARR_4S,
    MARR_2D
} MVecArr;

typedef enum {
    MADDR_OFFSET = 0,   /* [base, #imm] */
    MADDR_PRE_INDEX,    /* [base, #imm]! */
    MADDR_POST_INDEX    /* [base], #imm */
} MAddrMode;

typedef enum {
    /* Target-independent pseudo-ops (resolved by regalloc & frame layout) */
    MI_NOP = 0,
    MI_COPY,            /* dst = src (reg-to-reg, width in inst.width: 4, 8, 16) */
    MI_PCOPY,           /* Parallel copy bundle (index in inst.aux_id) */
    MI_FRAME_ADDR,      /* dst = &slot[slot_id] + imm */
    MI_LOAD_SLOT,       /* dst = *(T*)(&slot[slot_id] + imm) */
    MI_STORE_SLOT,      /* *(T*)(&slot[slot_id] + imm) = src */
    MI_LOAD_INARG,      /* dst = *(T*)(caller_sp + imm) */
    MI_INARG_ADDR,      /* dst = &caller_sp[imm] (SysV x64 inline stack byval) */
    MI_STORE_OUTARG,    /* *(T*)(sp + imm) = src */
    MI_COPY_OUTARG_MEM, /* memcpy(sp + imm, src_ptr, aux_id) (SysV x64 inline stack byval) */
    MI_VA_START,        /* init va_list at ptr operand[0] */
    MI_CALL_ADJ_DOWN,   /* if has_alloca_dyn: sub sp, sp, #imm before call */
    MI_CALL_ADJ_UP,     /* if has_alloca_dyn: add sp, sp, #imm after call */

    /* ARM64 Integer / ALU */
    MI_A64_MOVZ,        /* dst, #imm16, lsl #shift */
    MI_A64_MOVN,        /* dst, #imm16, lsl #shift */
    MI_A64_MOVK,        /* dst, #imm16, lsl #shift (updates dst in place) */
    MI_A64_MOV_RR,      /* mov dst, src (32 or 64) */
    MI_A64_ADD_RR,      /* add dst, rn, rm */
    MI_A64_ADD_RI,      /* add dst, rn, #imm12 (with optional lsl #12 in aux) */
    MI_A64_ADDS_RR,     /* adds dst, rn, rm */
    MI_A64_ADDS_RI,     /* adds dst, rn, #imm12 */
    MI_A64_SUB_RR,      /* sub dst, rn, rm */
    MI_A64_SUB_RI,      /* sub dst, rn, #imm12 */
    MI_A64_SUBS_RR,     /* subs dst, rn, rm */
    MI_A64_SUBS_RI,     /* subs dst, rn, #imm12 */
    MI_A64_ADC,         /* adc dst, rn, rm */
    MI_A64_ADCS,        /* adcs dst, rn, rm */
    MI_A64_SBC,         /* sbc dst, rn, rm */
    MI_A64_SBCS,        /* sbcs dst, rn, rm */
    MI_A64_MUL,         /* mul dst, rn, rm */
    MI_A64_MADD,        /* madd dst, rn, rm, ra  (ra + rn * rm) */
    MI_A64_MSUB,        /* msub dst, rn, rm, ra  (ra - rn * rm) */
    MI_A64_SMULL,       /* smull xd, wn, wm */
    MI_A64_UMULL,       /* umull xd, wn, wm */
    MI_A64_SMULH,       /* smulh xd, xn, xm */
    MI_A64_UMULH,       /* umulh xd, xn, xm */
    MI_A64_SDIV,        /* sdiv dst, rn, rm */
    MI_A64_UDIV,        /* udiv dst, rn, rm */
    MI_A64_AND_RR,      /* and dst, rn, rm */
    MI_A64_ORR_RR,      /* orr dst, rn, rm */
    MI_A64_EOR_RR,      /* eor dst, rn, rm */
    MI_A64_AND_RI,      /* and dst, rn, #bitmask */
    MI_A64_ORR_RI,      /* orr dst, rn, #bitmask */
    MI_A64_EOR_RI,      /* eor dst, rn, #bitmask */
    MI_A64_MVN,         /* mvn dst, rm */
    MI_A64_LSL_RR,      /* lslv dst, rn, rm */
    MI_A64_LSR_RR,      /* lsrv dst, rn, rm */
    MI_A64_ASR_RR,      /* asrv dst, rn, rm */
    MI_A64_ROR_RR,      /* rorv dst, rn, rm */
    MI_A64_LSL_RI,      /* lsl dst, rn, #imm */
    MI_A64_LSR_RI,      /* lsr dst, rn, #imm */
    MI_A64_ASR_RI,      /* asr dst, rn, #imm */
    MI_A64_ROR_RI,      /* ror dst, rn, #imm */
    MI_A64_CLZ,         /* clz dst, rn */
    MI_A64_RBIT,        /* rbit dst, rn */
    MI_A64_REV,         /* rev dst, rn (32 or 64) */
    MI_A64_REV16,       /* rev16 dst, rn */
    MI_A64_SXTB,        /* sxtb dst, wn */
    MI_A64_SXTH,        /* sxth dst, wn */
    MI_A64_SXTW,        /* sxtw xd, wn */
    MI_A64_UXTB,        /* uxtb wd, wn */
    MI_A64_UXTH,        /* uxth wd, wn */
    MI_A64_CMP_RR,      /* cmp rn, rm */
    MI_A64_CMP_RI,      /* cmp rn, #imm12 */
    MI_A64_CMP_SXTW,    /* cmp xn, wm, sxtw */
    MI_A64_CMP_ASR63,   /* cmp xn, xm, asr #63 */
    MI_A64_TST_RR,      /* tst rn, rm */
    MI_A64_CSET,        /* cset dst, cond */
    MI_A64_CSEL,        /* csel dst, rn, rm, cond */

    /* ARM64 Memory, Stack, Symbols, TLS & Atomics */
    MI_A64_LDR,         /* ldr[b/h/w/x/s/d/q] dst, [base, #imm] (unsigned/signed) */
    MI_A64_LDR_REG,     /* ldr[b/h/w/x/s/d/q] dst, [base, idx, lsl #shift] */
    MI_A64_STR,         /* str[b/h/w/x/s/d/q] src, [base, #imm] */
    MI_A64_STR_REG,     /* str[b/h/w/x/s/d/q] src, [base, idx, lsl #shift] */
    MI_A64_LDP,         /* ldp r1, r2, [base, #imm] (offset/post) */
    MI_A64_STP,         /* stp r1, r2, [base, #imm] (offset/pre) */
    MI_A64_MOV_SP,      /* mov dst, sp (aux=0) or mov sp, src (aux=1) */
    MI_A64_SYM_ADDR,    /* materialize address of global/func symbol (PIC PAGE/GOT) */
    MI_A64_TLS_ADDR,    /* materialize TLS address into x0 (clobbers caller-saved on Darwin) */
    MI_A64_LDAR,        /* ldar[b/h/w/x] dst, [base] */
    MI_A64_STLR,        /* stlr[b/h/w/x] src, [base] */
    MI_A64_DMB,         /* dmb ish / ishld / ishst (aux = PirMemOrder) */
    MI_A64_ATOMIC_RMW,  /* LL/SC atomic rmw loop (dst_old, ptr, val; aux=rmw_op, order) */
    MI_A64_ATOMIC_CAS,  /* LL/SC atomic cmpxchg loop (dst_old, dst_ok, ptr, exp, des) */

    /* ARM64 Scalar Floating-Point */
    MI_A64_FMOV_RR,     /* fmov s/d or mov v.16b */
    MI_A64_FMOV_G2F,    /* fmov s/d, w/x */
    MI_A64_FMOV_F2G,    /* fmov w/x, s/d */
    MI_A64_FADD,        /* fadd s/d */
    MI_A64_FSUB,        /* fsub s/d */
    MI_A64_FMUL,        /* fmul s/d */
    MI_A64_FDIV,        /* fdiv s/d */
    MI_A64_FNEG,        /* fneg s/d */
    MI_A64_FABS,        /* fabs s/d */
    MI_A64_FSQRT,       /* fsqrt s/d */
    MI_A64_FCMP,        /* fcmp sn/dn, sm/dm */
    MI_A64_FCSEL,       /* fcsel sd/dd, sn/dn, sm/dm, cond */
    MI_A64_FCVT,        /* fcvt d->s (width=4) or s->d (width=8) */
    MI_A64_SCVTF,       /* scvtf dst_fp, src_gp (aux=src_width) */
    MI_A64_UCVTF,       /* ucvtf dst_fp, src_gp (aux=src_width) */
    MI_A64_FCVTZS,      /* fcvtzs dst_gp, src_fp (aux=src_fp_width) */
    MI_A64_FCVTZU,      /* fcvtzu dst_gp, src_fp (aux=src_fp_width) */

    /* ARM64 NEON SIMD */
    MI_A64_VADD,        /* add vd.arr, vn.arr, vm.arr */
    MI_A64_VSUB,        /* sub vd.arr, vn.arr, vm.arr */
    MI_A64_VMUL,        /* mul vd.arr, vn.arr, vm.arr */
    MI_A64_VAND,        /* and vd.16b, vn.16b, vm.16b */
    MI_A64_VORR,        /* orr vd.16b, vn.16b, vm.16b */
    MI_A64_VEOR,        /* eor vd.16b, vn.16b, vm.16b */
    MI_A64_VMVN,        /* not vd.16b, vn.16b */
    MI_A64_VBSL,        /* bsl vd.16b, vn.16b, vm.16b (in-place on vd!) */
    MI_A64_VBIT,        /* bit vd.16b, vn.16b, vm.16b (in-place on vd = fv, vn = tv, vm = mask) */
    MI_A64_VFADD,       /* fadd vd.arr, vn.arr, vm.arr */
    MI_A64_VFSUB,       /* fsub vd.arr, vn.arr, vm.arr */
    MI_A64_VFMUL,       /* fmul vd.arr, vn.arr, vm.arr */
    MI_A64_VFDIV,       /* fdiv vd.arr, vn.arr, vm.arr */
    MI_A64_VFMIN,       /* fmin vd.arr, vn.arr, vm.arr */
    MI_A64_VFMAX,       /* fmax vd.arr, vn.arr, vm.arr */
    MI_A64_VFNEG,       /* fneg vd.arr, vn.arr */
    MI_A64_VCMEQ,       /* cmeq vd.arr, vn.arr, vm.arr */
    MI_A64_VCMGT,       /* cmgt vd.arr, vn.arr, vm.arr */
    MI_A64_VCMGE,       /* cmge vd.arr, vn.arr, vm.arr */
    MI_A64_VCMHI,       /* cmhi vd.arr, vn.arr, vm.arr */
    MI_A64_VCMHS,       /* cmhs vd.arr, vn.arr, vm.arr */
    MI_A64_VFCMEQ,      /* fcmeq vd.arr, vn.arr, vm.arr */
    MI_A64_VFCMGT,      /* fcmgt vd.arr, vn.arr, vm.arr */
    MI_A64_VFCMGE,      /* fcmge vd.arr, vn.arr, vm.arr */
    MI_A64_VDUP_GP,     /* dup vd.arr, wn/xn */
    MI_A64_VDUP_LANE,   /* dup vd.arr, vn.elem[0] */
    MI_A64_VUMOV,       /* umov wd/xd, vn.elem[lane] */
    MI_A64_VFMOV_LANE,  /* dup/mov s/d dst, vn.elem[lane] */
    MI_A64_VINS_GP,     /* ins vd.elem[lane], wn/xn (read-modify-write vd) */
    MI_A64_VINS_LANE,   /* ins vd.elem[dst_lane], vn.elem[src_lane] */
    MI_A64_VSHL_S,      /* sshl vd.arr, vn.arr, vm.arr */
    MI_A64_VSHL_U,      /* ushl vd.arr, vn.arr, vm.arr */
    MI_A64_VNEG,        /* neg vd.arr, vn.arr */
    MI_A64_VCNT,        /* cnt vd.8b, vn.8b */
    MI_A64_VADDV,       /* addv bd, vn.8b */
    MI_A64_VSCVTF,      /* scvtf vd.arr, vn.arr */
    MI_A64_VUCVTF,      /* ucvtf vd.arr, vn.arr */
    MI_A64_VFCVTZS,     /* fcvtzs vd.arr, vn.arr */
    MI_A64_VFCVTZU,     /* fcvtzu vd.arr, vn.arr */

    /* ARM64 Control Flow */
    MI_A64_B,           /* b target_block */
    MI_A64_BCOND,       /* b.<cond> target_block */
    MI_A64_CBZ,         /* cbz rn, target_block */
    MI_A64_CBNZ,        /* cbnz rn, target_block */
    MI_A64_BL,          /* bl symbol (call clobbers!) */
    MI_A64_BLR,         /* blr rn (call clobbers!) */
    MI_A64_SWITCH_TBL,  /* PC-relative jump table dispatch (val, jtable_id, default_blk) */
    MI_A64_RET,         /* ret (epilogue marker before frame pass) */
    MI_A64_UDF,         /* udf #0 (deterministic SIGILL / exit 132 trap) */

    /* x86-64 Data Movement, Memory & Addressing */
    MI_X64_MOV_RR,      /* mov dst, src (32 or 64) */
    MI_X64_MOV_RI,      /* mov dst, imm64 (32 or 64) */
    MI_X64_MOVSX,       /* movsx/movsxd dst, src (aux_width = 1, 2, or 4 -> width 4 or 8) */
    MI_X64_MOVZX,       /* movzx dst, src (aux_width = 1 or 2 -> width 4) */
    MI_X64_MOV_RM,      /* mov/movzx/movsx dst, [base + disp] */
    MI_X64_MOV_RM_IDX,  /* mov/movzx/movsx dst, [base + idx*scale + disp] */
    MI_X64_MOV_MR,      /* mov [base + disp], src */
    MI_X64_MOV_MR_IDX,  /* mov [base + idx*scale + disp], src */
    MI_X64_LEA,         /* lea dst, [base + disp] */
    MI_X64_LEA_IDX,     /* lea dst, [base + idx*scale + disp] */
    MI_X64_PUSH,        /* push reg */
    MI_X64_POP,         /* pop reg */
    MI_X64_MOV_RSP,     /* mov dst, rsp (aux_id=0) or mov rsp, src (aux_id=1) */
    MI_X64_SYM_ADDR,    /* PIC RIP-relative lea or GOT load */
    MI_X64_TLS_ADDR,    /* Mach-O @TLVP thunk call (returns rax) or Linux fs:[0]+@TPOFF */

    /* x86-64 Integer ALU (2-address: ops[0] is defuse) */
    MI_X64_ADD_RR,
    MI_X64_ADD_RI,
    MI_X64_SUB_RR,
    MI_X64_SUB_RI,
    MI_X64_ADC_RR,
    MI_X64_SBB_RR,
    MI_X64_IMUL_RR,     /* imul dst, src (2-address) */
    MI_X64_IMUL_RRI,    /* imul dst, src, imm32 (3-address) */
    MI_X64_MUL_1,       /* mul src (rdx:rax = rax * src, unsigned) */
    MI_X64_IMUL_1,      /* imul src (rdx:rax = rax * src, signed) */
    MI_X64_CQO,         /* cdq (width=4) / cqo (width=8) */
    MI_X64_IDIV,        /* idiv src (rdx:rax / src -> rax=q, rdx=r) */
    MI_X64_DIV,         /* div src (rdx:rax / src -> rax=q, rdx=r) */
    MI_X64_AND_RR,
    MI_X64_AND_RI,
    MI_X64_OR_RR,
    MI_X64_OR_RI,
    MI_X64_XOR_RR,
    MI_X64_XOR_RI,
    MI_X64_NEG,
    MI_X64_NOT,
    MI_X64_SHL_RI,
    MI_X64_SHR_RI,
    MI_X64_SAR_RI,
    MI_X64_ROR_RI,
    MI_X64_SHL_CL,
    MI_X64_SHR_CL,
    MI_X64_SAR_CL,
    MI_X64_ROR_CL,
    MI_X64_BSR,
    MI_X64_BSF,
    MI_X64_BSWAP,
    MI_X64_CMP_RR,
    MI_X64_CMP_RI,
    MI_X64_TEST_RR,
    MI_X64_TEST_RI,
    MI_X64_SETCC,       /* set<cc> dst8; movzx dst32, dst8 */
    MI_X64_CMOVCC,      /* cmov<cc> dst, src */

    /* x86-64 Atomics & Fences */
    MI_X64_MFENCE,
    MI_X64_ATOMIC_RMW,  /* xchg / lock xadd / lock cmpxchg loop */
    MI_X64_ATOMIC_CAS,  /* lock cmpxchg [ptr], des + sete dst_ok (rax=exp/old) */

    /* x86-64 SSE2 Scalar Floating-Point & Conversions */
    MI_X64_MOVX_RR,     /* movaps dst, src */
    MI_X64_MOVX_RM,     /* movss/movsd/movups dst, [base + disp] */
    MI_X64_MOVX_RM_IDX, /* movss/movsd/movups dst, [base + idx*scale + disp] */
    MI_X64_MOVX_MR,     /* movss/movsd/movups [base + disp], src */
    MI_X64_MOVX_MR_IDX, /* movss/movsd/movups [base + idx*scale + disp], src */
    MI_X64_MOV_G2X,     /* movd/movq xmm, gp */
    MI_X64_MOV_X2G,     /* movd/movq gp, xmm */
    MI_X64_ADDSS,
    MI_X64_ADDSD,
    MI_X64_SUBSS,
    MI_X64_SUBSD,
    MI_X64_MULSS,
    MI_X64_MULSD,
    MI_X64_DIVSS,
    MI_X64_DIVSD,
    MI_X64_SQRTSS,
    MI_X64_SQRTSD,
    MI_X64_UCOMISS,
    MI_X64_UCOMISD,
    MI_X64_CVTSS2SD,
    MI_X64_CVTSD2SS,
    MI_X64_CVTSI2SS,
    MI_X64_CVTSI2SD,
    MI_X64_CVTTSS2SI,
    MI_X64_CVTTSD2SI,

    /* x86-64 SSE2 Vector Ops (2-address: ops[0] is defuse except pshufd/cvt) */
    MI_X64_PADDB,
    MI_X64_PADDW,
    MI_X64_PADDD,
    MI_X64_PADDQ,
    MI_X64_PSUBB,
    MI_X64_PSUBW,
    MI_X64_PSUBD,
    MI_X64_PSUBQ,
    MI_X64_PMULLW,
    MI_X64_PAND,
    MI_X64_POR,
    MI_X64_PXOR,
    MI_X64_PANDN,
    MI_X64_ADDPS,
    MI_X64_ADDPD,
    MI_X64_SUBPS,
    MI_X64_SUBPD,
    MI_X64_MULPS,
    MI_X64_MULPD,
    MI_X64_DIVPS,
    MI_X64_DIVPD,
    MI_X64_MINPS,
    MI_X64_MINPD,
    MI_X64_MAXPS,
    MI_X64_MAXPD,
    MI_X64_PCMPEQB,
    MI_X64_PCMPEQW,
    MI_X64_PCMPEQD,
    MI_X64_PCMPGTB,
    MI_X64_PCMPGTW,
    MI_X64_PCMPGTD,
    MI_X64_CMPPS,
    MI_X64_CMPPD,
    MI_X64_PSHUFD,
    MI_X64_PUNPCKLBW,
    MI_X64_PUNPCKLWD,
    MI_X64_PUNPCKLDQ,
    MI_X64_PUNPCKLQDQ,
    MI_X64_CVTDQ2PS,
    MI_X64_CVTTPS2DQ,

    /* x86-64 Control Flow */
    MI_X64_JMP,
    MI_X64_JCC,
    MI_X64_CALL,
    MI_X64_CALL_IND,
    MI_X64_SWITCH_TBL,
    MI_X64_RET,
    MI_X64_UD2
} MOpcode;

/* =========================================================================
 * 3. Operands, Parallel Copies, Stack Slots, Jump Tables, Instructions
 * ========================================================================= */

typedef enum {
    MOP_NONE = 0,
    MOP_VREG,
    MOP_PREG,
    MOP_IMM,
    MOP_SYM,
    MOP_BLOCK,
    MOP_SLOT,
    MOP_JTABLE,
    MOP_COND
} MOperandKind;

typedef struct {
    MOperandKind kind;
    bool         is_def;
    bool         is_use;
    bool         is_early_clobber;
    union {
        MVReg       vreg;
        MPReg       preg;
        int64_t     imm;
        uint32_t    block_id;
        uint32_t    slot_id;
        uint32_t    jtable_id;
        MCond       cond;
        struct {
            const char *name;
            int64_t     offset;
            bool        is_func;
            bool        is_tls;
            bool        use_got; /* extern or weak symbol -> GOT on Mach-O/ELF */
        } sym;
    } u;
} MOperand;

#define MINST_MAX_OPS 6u

typedef struct {
    MOpcode    op;
    uint8_t    width;        /* Primary width in bytes: 1, 2, 4, 8, or 16 */
    uint8_t    aux_width;    /* Secondary width / shift / lane index */
    uint8_t    aux_lane2;    /* Second lane index (for VINS_LANE) */
    MVecArr    arr;          /* NEON arrangement */
    MAddrMode  addr_mode;    /* Offset / pre-index / post-index */
    bool       is_signed;    /* Signed load/ext if true */
    bool       is_call;      /* Clobbers caller-saved registers */
    uint32_t   aux_id;       /* PCopy bundle index / RMW op / mem order */
    uint32_t   aux_id2;      /* Second order (cmpxchg fail_order) */
    uint32_t   loc_idx;      /* PIR source debug location index (0 = none) */
    uint8_t    op_count;
    MOperand   ops[MINST_MAX_OPS];
} MInst;

typedef struct {
    MVReg      dst;          /* Destination virtual register */
    bool       src_is_imm;   /* If true, copy immediate instead of vreg */
    MVReg      src;          /* Source virtual register (when !src_is_imm) */
    uint64_t   imm_bits;     /* Immediate bits (when src_is_imm) */
    MRegClass  rclass;
    uint8_t    byte_size;
} MPCopyEntry;

typedef struct {
    MPCopyEntry *entries;
    uint32_t     count;
} MPCopyBundle;

typedef struct {
    MRegClass  rclass;
    uint8_t    byte_size;    /* 4, 8, or 16 (GP: 4 or 8; FP: 4, 8, or 16) */
    MPReg      fixed_preg;   /* MREG_NONE if unpinned */
    MPReg      hint_preg;    /* Preferred preg (MREG_NONE if none) */
    MPReg      assigned_preg;/* Filled by regalloc (MREG_NONE if spilled) */
    int32_t    spill_slot;   /* Stack slot ID if spilled, else -1 */
} MVRegInfo;

typedef struct {
    uint64_t   size;
    uint32_t   align;
    int64_t    fp_offset;    /* Negative offset from x29 (filled by frame pass) */
    bool       is_spill;
} MStackSlot;

typedef struct {
    int64_t    min_val;
    uint32_t   num_entries;
    uint32_t  *targets;      /* Array of MBlock IDs */
    uint32_t   default_blk;
} MJTable;

typedef PIR_VEC(MInst) MInstVec;

typedef struct {
    uint32_t       id;
    const char    *label;    /* Original PIR label or generated name */
    MInstVec       insts;
    PIR_VEC(uint32_t) succs;
    PIR_VEC(uint32_t) preds;
} MBlock;

typedef struct {
    PirArena              arena;
    const PirFunction    *pir_fn;
    uint32_t              fn_index;
    const PirTargetSpec  *target;

    PIR_VEC(MVRegInfo)    vregs;
    PIR_VEC(MBlock)       blocks;
    PIR_VEC(MStackSlot)   slots;
    PIR_VEC(MPCopyBundle) pcopies;
    PIR_VEC(MJTable)      jtables;

    /* Frame & ABI metadata */
    bool                  has_calls;
    bool                  has_alloca_dyn;
    bool                  is_variadic;
    uint32_t              va_fixed_gp;
    uint32_t              va_fixed_fp;
    uint64_t              va_fixed_stack_bytes; /* Offset of first vararg on stack */
    uint64_t              max_out_args_bytes;   /* Max outgoing stack arg bytes across calls */
    uint32_t              max_out_args_align;   /* Max outgoing stack arg alignment (>16 requires RSP realignment) */

    /* Filled by regalloc */
    uint32_t              used_callee_gp_mask;  /* Bitmask of GP regs 19..28 */
    uint32_t              used_callee_fp_mask;  /* Bitmask of FP regs 8..15 (d8..d15) */

    /* Filled by arm64_frame / x64_frame */
    uint64_t              local_frame_size;     /* 16-byte aligned sub sp size */
    int64_t               linux_gr_top_off;     /* Negative fp offset for Linux ARM64 va_start */
    int64_t               linux_vr_top_off;
    int64_t               x64_reg_save_off;     /* Negative rbp offset for SysV x64 va_start */
} MFunc;

/* =========================================================================
 * 4. MFunc Construction Helpers
 * ========================================================================= */

void     mfunc_init(MFunc *mf, const PirFunction *pir_fn, uint32_t fn_index,
                    const PirTargetSpec *target);
void     mfunc_destroy(MFunc *mf);

MVReg    mfunc_new_vreg(MFunc *mf, MRegClass rc, uint8_t byte_size);
uint32_t mfunc_new_block(MFunc *mf, const char *label);
uint32_t mfunc_add_slot(MFunc *mf, uint64_t size, uint32_t align, bool is_spill);
uint32_t mfunc_add_pcopy(MFunc *mf, const MPCopyEntry *entries, uint32_t count);
uint32_t mfunc_add_jtable(MFunc *mf, int64_t min_val, uint32_t num_entries,
                          const uint32_t *targets, uint32_t default_blk);
void     mfunc_add_edge(MFunc *mf, uint32_t from_blk, uint32_t to_blk);
void     mblock_push(MBlock *blk, MInst inst);

/* Operand constructors */
MInst    minst_make(MOpcode op, uint8_t width, uint32_t loc_idx);
MOperand mop_vreg_def(MVReg v);
MOperand mop_vreg_use(MVReg v);
MOperand mop_vreg_def_ec(MVReg v);
MOperand mop_vreg_defuse(MVReg v);
MOperand mop_preg_def(MPReg p);
MOperand mop_preg_use(MPReg p);
MOperand mop_preg_defuse(MPReg p);
MOperand mop_imm(int64_t val);
MOperand mop_sym(const char *name, int64_t off, bool is_func, bool is_tls, bool use_got);
MOperand mop_block(uint32_t blk_id);
MOperand mop_slot(uint32_t slot_id);
MOperand mop_jtable(uint32_t jt_id);
MOperand mop_cond(MCond c);

#endif /* PIR_BACKEND_MIR_H */
