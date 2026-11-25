/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.5 / §6.5.7 / §6.8.4.2 — Adversarial test for
 * x86-64 fixed-register constraints (rax/rdx idiv/div, cl variable shifts &
 * rotates, 4th SysV arg rcx contention), movabs 64-bit immediates, high-range
 * switch jump tables, and 14+ live GP register spill pressure.
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

static uint64_t opaque_u64(uint64_t v) {
    volatile uint64_t x = v;
    return x;
}

static int64_t opaque_i64(int64_t v) {
    volatile int64_t x = v;
    return x;
}

/* 4th arg on SysV AMD64 arrives in RCX: stress RCX as BOTH shifted value AND shift count */
static uint64_t rcx_shift_collide(uint64_t a, uint64_t b, uint64_t c, uint64_t d_in_rcx,
                                  unsigned s1, unsigned s2) {
    uint64_t r1 = (d_in_rcx << s1) ^ (a >> (d_in_rcx & 15u));
    uint64_t r2 = (b << (c & 15u)) ^ (d_in_rcx >> s2);
    uint64_t r3 = ((d_in_rcx >> s1) | (d_in_rcx << (64u - s1)));
    uint64_t r4 = ((c >> s2) | (c << (64u - s2)));
    return r1 + r2 + r3 + r4 + d_in_rcx;
}

/* 32-bit variable rotate & shift with RCX contention and 12+ live GP values */
static uint32_t shift_pressure_32(uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                                  unsigned n1, unsigned n2) {
    uint32_t v0 = a + 1u, v1 = b + 2u, v2 = c + 3u, v3 = d + 4u;
    uint32_t v4 = a ^ 0x55555555u, v5 = b ^ 0xAAAAAAAAu;
    uint32_t v6 = c * 3u, v7 = d * 5u;
    uint32_t v8 = (v0 >> n1) | (v0 << (32u - n1));
    uint32_t v9 = (v3 >> n2) | (v3 << (32u - n2));
    int32_t s_neg = -(int32_t)(v1 | 0x40000000u);
    int32_t sar1 = s_neg >> (int)(n1 & 7u);
    int32_t sar2 = s_neg >> (int)(n2 & 7u);
    uint32_t v10 = (v4 << (n1 & 15u)) ^ (v5 >> (n2 & 15u));
    uint32_t v11 = (v6 << (n2 & 7u)) ^ (v7 >> (n1 & 7u));
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11
         + (uint32_t)sar1 + (uint32_t)sar2;
}

/* Interleaved signed/unsigned 64-bit & 32-bit div/rem stressing RAX/RDX clobbers & spills */
static int64_t div_rem_stress(int64_t a, int64_t b, int64_t c, int64_t d,
                              uint64_t ua, uint64_t ub) {
    int64_t q1 = a / b;
    int64_t r1 = a % b;
    int64_t q2 = c / d;
    int64_t r2 = c % d;
    /* Use previous quotient/remainder as next divisors (forces rax/rdx shuffle!) */
    int64_t div_a = (q1 != 0) ? q1 : 1;
    int64_t div_b = (r2 != 0) ? r2 : 1;
    int64_t q3 = (a + c) / div_a;
    int64_t r3 = (b + d) % div_b;
    uint64_t uq = ua / ub;
    uint64_t ur = ua % ub;
    int32_t sq32 = (int32_t)a / (int32_t)b;
    int32_t sr32 = (int32_t)c % (int32_t)d;
    uint32_t uq32 = (uint32_t)ua / (uint32_t)ub;
    uint32_t ur32 = (uint32_t)ua % (uint32_t)ub;
    /* Keep all originals live across every idiv/div */
    return a + b + c + d + q1 + r1 + q2 + r2 + q3 + r3
         + (int64_t)(uq ^ ur) + (int64_t)sq32 + (int64_t)sr32
         + (int64_t)(uq32 + ur32);
}

/* Subword signed/unsigned variable shifts & divisions */
static int subword_shift_div(int8_t s8, uint8_t u8, int16_t s16, uint16_t u16, unsigned k) {
    int8_t d8 = (int8_t)(s8 / (int8_t)-3);
    int8_t m8 = (int8_t)(s8 % (int8_t)-3);
    uint8_t ud8 = (uint8_t)(u8 / 7u);
    uint8_t um8 = (uint8_t)(u8 % 7u);
    int16_t d16 = (int16_t)(s16 / (int16_t)-17);
    int16_t m16 = (int16_t)(s16 % (int16_t)-17);
    uint16_t ud16 = (uint16_t)(u16 / 13u);
    uint16_t um16 = (uint16_t)(u16 % 13u);
    int sh_u8 = (int)(u8 >> (k & 3u));
    int sh_s16 = (int)(s16 >> (k & 3u));
    return (int)d8 + (int)m8 + (int)ud8 + (int)um8
         + (int)d16 + (int)m16 + (int)ud16 + (int)um16
         + sh_u8 + sh_s16;
}

/* 64-bit immediates (> INT32_MAX, < INT32_MIN) in ALU, comparisons, and dense/sparse switch */
static int switch_high64_dense(uint64_t x) {
    switch (x) {
        case 0x100000000ULL: return 10;
        case 0x100000001ULL: return 20;
        case 0x100000002ULL: return 30;
        case 0x100000003ULL: return 40;
        case 0x100000004ULL: return 50;
        default:             return -1;
    }
}

static int switch_int64_min_dense(int64_t x) {
    switch (x) {
        case INT64_MIN:      return 101;
        case INT64_MIN + 1:  return 102;
        case INT64_MIN + 2:  return 103;
        case INT64_MIN + 3:  return 104;
        default:             return -2;
    }
}

static int switch_u32_high_dense(uint32_t x) {
    switch (x) {
        case 0xFFFFFFFCU: return 1;
        case 0xFFFFFFFDU: return 2;
        case 0xFFFFFFFEU: return 3;
        case 0xFFFFFFFFU: return 4;
        default:          return 0;
    }
}

static uint64_t movabs_alu_ops(uint64_t x) {
    uint64_t a = x + 0x123456789ABCDEF0ULL;
    uint64_t b = a ^ 0xFEDCBA9876543210ULL;
    uint64_t c = b & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t d = c | 0x8000000000000001ULL;
    uint64_t e = (d > 0x8000000100000000ULL) ? (d - 0x0000000100000000ULL)
                                             : (d + 0x0000000200000000ULL);
    return e;
}

int main(void) {
    C17_BEGIN("c65_bug_x64_fixed_reg_clobbers",
              "x86-64 fixed-reg idiv/div, rcx shift/rotate, movabs, high-64 switch");

    /* 1. RCX shift/rotate collision */
    {
        uint64_t a = opaque_u64(0x0123456789ABCDEFULL);
        uint64_t b = opaque_u64(0xF0E1D2C3B4A59687ULL);
        uint64_t c = opaque_u64(0x13579BDF2468ACE0ULL);
        uint64_t d = opaque_u64(0xCAFEBABEDEADBEEFULL);
        uint64_t got = rcx_shift_collide(a, b, c, d,
                                         (unsigned)c17_opaque_int(7),
                                         (unsigned)c17_opaque_int(19));
        uint64_t want = ((d << 7) ^ (a >> (d & 15u)))
                      + ((b << (c & 15u)) ^ (d >> 19))
                      + ((d >> 7) | (d << 57))
                      + ((c >> 19) | (c << 45))
                      + d;
        C17_CHECK_UEQ(got, want);
    }

    /* 2. 32-bit variable rotate + arithmetic right shift under high GP pressure */
    {
        uint32_t a = (uint32_t)c17_opaque_int(0x12345678);
        uint32_t b = (uint32_t)c17_opaque_int(0x0F0F0F0F);
        uint32_t c = (uint32_t)c17_opaque_int(0x11223344);
        uint32_t d = (uint32_t)c17_opaque_int(0x55667788);
        uint32_t got = shift_pressure_32(a, b, c, d,
                                         (unsigned)c17_opaque_int(5),
                                         (unsigned)c17_opaque_int(13));
        uint32_t v0 = a + 1u, v1 = b + 2u, v2 = c + 3u, v3 = d + 4u;
        uint32_t v4 = a ^ 0x55555555u, v5 = b ^ 0xAAAAAAAAu;
        uint32_t v6 = c * 3u, v7 = d * 5u;
        uint32_t v8 = (v0 >> 5) | (v0 << 27);
        uint32_t v9 = (v3 >> 13) | (v3 << 19);
        int32_t s_neg = -(int32_t)(v1 | 0x40000000u);
        uint32_t want = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9
                      + ((v4 << 5) ^ (v5 >> 13))
                      + ((v6 << 5) ^ (v7 >> 5))
                      + (uint32_t)(s_neg >> 5) + (uint32_t)(s_neg >> 5);
        C17_CHECK_UEQ(got, want);
    }

    /* 3. Nested idiv/div with rax/rdx contention & negative operands */
    {
        int64_t a = opaque_i64(-1000000007LL);
        int64_t b = opaque_i64(137LL);
        int64_t c = opaque_i64(987654321LL);
        int64_t d = opaque_i64(-29LL);
        uint64_t ua = opaque_u64(0xFFFFFFFFFFFFFFF0ULL);
        uint64_t ub = opaque_u64(0x100000003ULL);
        int64_t got = div_rem_stress(a, b, c, d, ua, ub);
        int64_t q1 = a / b, r1 = a % b, q2 = c / d, r2 = c % d;
        int64_t q3 = (a + c) / q1, r3 = (b + d) % r2;
        uint64_t uq = ua / ub, ur = ua % ub;
        int64_t want = a + b + c + d + q1 + r1 + q2 + r2 + q3 + r3
                     + (int64_t)(uq ^ ur)
                     + (int64_t)((int32_t)a / (int32_t)b)
                     + (int64_t)((int32_t)c % (int32_t)d)
                     + (int64_t)(((uint32_t)ua / (uint32_t)ub) + ((uint32_t)ua % (uint32_t)ub));
        C17_CHECK_EQ(got, want);

        /* Boundary signed 64-bit / -1 (non-UB: INT64_MIN + 1) */
        int64_t mn1 = opaque_i64(INT64_MIN + 1);
        int64_t neg1 = opaque_i64(-1);
        C17_CHECK_EQ(mn1 / neg1, INT64_MAX);
        C17_CHECK_EQ(mn1 % neg1, 0LL);
        C17_CHECK_EQ(opaque_i64(INT64_MIN) / opaque_i64(2), -(1LL << 62));
    }

    /* 4. Subword shifts & divisions */
    {
        int got = subword_shift_div((int8_t)c17_opaque_int(-100),
                                    (uint8_t)c17_opaque_int(250),
                                    (int16_t)c17_opaque_int(-12345),
                                    (uint16_t)c17_opaque_int(60000),
                                    (unsigned)c17_opaque_int(2));
        /* d8=33, m8=-1, ud8=35, um8=5, d16=726, m16=-3, ud16=4615, um16=5,
         * sh_u8=62, sh_s16=-3087 -> sum = 2390 */
        C17_CHECK_EQ(got, 2390);
    }

    /* 5. High-64 and INT64_MIN jump-table switches & movabs ALU */
    {
        C17_CHECK_EQ(switch_high64_dense(opaque_u64(0x100000000ULL)), 10);
        C17_CHECK_EQ(switch_high64_dense(opaque_u64(0x100000003ULL)), 40);
        C17_CHECK_EQ(switch_high64_dense(opaque_u64(0x100000005ULL)), -1);
        C17_CHECK_EQ(switch_high64_dense(opaque_u64(0ULL)), -1);

        C17_CHECK_EQ(switch_int64_min_dense(opaque_i64(INT64_MIN)), 101);
        C17_CHECK_EQ(switch_int64_min_dense(opaque_i64(INT64_MIN + 2)), 103);
        C17_CHECK_EQ(switch_int64_min_dense(opaque_i64(INT64_MAX)), -2);
        C17_CHECK_EQ(switch_int64_min_dense(opaque_i64(0)), -2);

        C17_CHECK_EQ(switch_u32_high_dense((uint32_t)c17_opaque_int((int)0xFFFFFFFCU)), 1);
        C17_CHECK_EQ(switch_u32_high_dense((uint32_t)c17_opaque_int((int)0xFFFFFFFFU)), 4);
        C17_CHECK_EQ(switch_u32_high_dense((uint32_t)c17_opaque_int(0)), 0);

        uint64_t x0 = opaque_u64(0x0000000100000001ULL);
        uint64_t a = x0 + 0x123456789ABCDEF0ULL;
        uint64_t b = a ^ 0xFEDCBA9876543210ULL;
        uint64_t c = b & 0x7FFFFFFFFFFFFFFFULL;
        uint64_t d = c | 0x8000000000000001ULL;
        uint64_t want = (d > 0x8000000100000000ULL) ? (d - 0x0000000100000000ULL)
                                                    : (d + 0x0000000200000000ULL);
        C17_CHECK_UEQ(movabs_alu_ops(x0), want);
    }

    C17_END();
}
