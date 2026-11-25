/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.8 / §6.5.9 / §6.3.1.4 — Adversarial test for
 * x86-64 SSE2 ucomisd/ucomiss parity flag (PF) handling on NaN, signed zero,
 * fused vs materialized comparisons, shared-condition multi-select (cmovcc),
 * FP ternary select, and uint64_t <-> float/double across the 2^63 split.
 */
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include "c17_test.h"

static float opaque_f32(float v) {
    volatile float x = v;
    return x;
}

static uint64_t opaque_u64(uint64_t v) {
    volatile uint64_t x = v;
    return x;
}

static int64_t opaque_i64(int64_t v) {
    volatile int64_t x = v;
    return x;
}

/* Materialized 6-bit mask: bit0=(==), bit1=(!=), bit2=(<), bit3=(<=), bit4=(>), bit5=(>=) */
static int cmp_mask_f64_mat(double a, double b) {
    int m = 0;
    m |= (a == b) ? 1 : 0;
    m |= (a != b) ? 2 : 0;
    m |= (a <  b) ? 4 : 0;
    m |= (a <= b) ? 8 : 0;
    m |= (a >  b) ? 16 : 0;
    m |= (a >= b) ? 32 : 0;
    return m;
}

static int cmp_mask_f32_mat(float a, float b) {
    int m = 0;
    m |= (a == b) ? 1 : 0;
    m |= (a != b) ? 2 : 0;
    m |= (a <  b) ? 4 : 0;
    m |= (a <= b) ? 8 : 0;
    m |= (a >  b) ? 16 : 0;
    m |= (a >= b) ? 32 : 0;
    return m;
}

/* Branch-based 6-bit mask (exercises fused ucomisd/ucomiss + jcc) */
static int cmp_mask_f64_br(double a, double b) {
    int m = 0;
    if (a == b) m |= 1;
    if (a != b) m |= 2;
    if (a <  b) m |= 4;
    if (a <= b) m |= 8;
    if (a >  b) m |= 16;
    if (a >= b) m |= 32;
    return m;
}

/* Negated predicates on NaN (must be TRUE for !(a==b), !(a<b), !(a<=b), !(a>b), !(a>=b) and FALSE for !(a!=b)!) */
static int cmp_neg_mask_f64(double a, double b) {
    int m = 0;
    if (!(a == b)) m |= 1;
    if (!(a != b)) m |= 2;
    if (!(a <  b)) m |= 4;
    if (!(a <= b)) m |= 8;
    if (!(a >  b)) m |= 16;
    if (!(a >= b)) m |= 32;
    return m;
}

/* Shared-condition multi-select (exercises x64 last_flags_cmp_id cmovcc reuse!) */
static int multi_select_f64(double a, double b, int x1, int y1, int x2, int y2, int x3, int y3) {
    int c_lt = (a < b);
    int r1 = c_lt ? x1 : y1;
    int r2 = c_lt ? x2 : y2;
    int r3 = c_lt ? x3 : y3;
    int c_eq = (a == b);
    int r4 = c_eq ? x1 : y2;
    int c_ne = (a != b);
    int r5 = c_ne ? x3 : y1;
    return r1 + r2 * 2 + r3 * 4 + r4 * 8 + r5 * 16;
}

/* Floating-point ternary select (exercises x64 fsel_t/fsel_f branch select) */
static double fp_select_f64(double a, double b, double tv, double fv) {
    double s1 = (a < b) ? tv : fv;
    double s2 = (a == b) ? tv : fv;
    double s3 = (a != b) ? tv : fv;
    return s1 + s2 * 10.0 + s3 * 100.0;
}

static uint64_t f64_to_u64(double d) { return (uint64_t)d; }
static uint64_t f32_to_u64(float f)  { return (uint64_t)f; }
static double u64_to_f64(uint64_t u) { return (double)u; }
static float  u64_to_f32(uint64_t u) { return (float)u; }
static uint32_t f64_to_u32(double d) { return (uint32_t)d; }
static double u32_to_f64(uint32_t u) { return (double)u; }

int main(void) {
    C17_BEGIN("c65_bug_x64_fp_nan_parity_and_select",
              "x86-64 ucomisd/ucomiss NaN PF, shared cmovcc select, u64<->fp 2^63 split");

    double qnan = c17_opaque_dbl(0.0) / c17_opaque_dbl(0.0);
    double nnan = -qnan;
    double pos0 = c17_opaque_dbl(0.0);
    double neg0 = -pos0;
    double one  = c17_opaque_dbl(1.0);
    double two  = c17_opaque_dbl(2.0);
    double inf  = c17_opaque_dbl(1.0) / c17_opaque_dbl(0.0);
    double ninf = -inf;

    /* 1. NaN: ONLY != (bit 1 = 2) is true! */
    C17_CHECK_EQ(cmp_mask_f64_mat(qnan, qnan), 2);
    C17_CHECK_EQ(cmp_mask_f64_mat(qnan, one),  2);
    C17_CHECK_EQ(cmp_mask_f64_mat(one,  nnan), 2);
    C17_CHECK_EQ(cmp_mask_f64_mat(qnan, inf),  2);
    C17_CHECK_EQ(cmp_mask_f64_br(qnan, qnan),  2);
    C17_CHECK_EQ(cmp_mask_f64_br(qnan, one),   2);
    C17_CHECK_EQ(cmp_mask_f64_br(one,  qnan),  2);
    /* Negated on NaN: everything EXCEPT !(a!=b) is true -> 1+4+8+16+32 = 61 */
    C17_CHECK_EQ(cmp_neg_mask_f64(qnan, qnan), 61);
    C17_CHECK_EQ(cmp_neg_mask_f64(qnan, one),  61);

    /* Float32 NaN */
    float fnan = opaque_f32(0.0f) / opaque_f32(0.0f);
    float fone = opaque_f32(1.0f);
    float ftwo = opaque_f32(2.0f);
    C17_CHECK_EQ(cmp_mask_f32_mat(fnan, fnan), 2);
    C17_CHECK_EQ(cmp_mask_f32_mat(fnan, fone), 2);
    C17_CHECK_EQ(cmp_mask_f32_mat(fone, ftwo), 2 + 4 + 8);
    C17_CHECK_EQ(cmp_mask_f32_mat(ftwo, fone), 2 + 16 + 32);

    /* 2. Signed zero & infinity */
    /* +0.0 == -0.0: bits 0 (==), 3 (<=), 5 (>=) -> 1 + 8 + 32 = 41 */
    C17_CHECK_EQ(cmp_mask_f64_mat(pos0, neg0), 41);
    C17_CHECK_EQ(cmp_mask_f64_br(pos0, neg0),  41);
    C17_CHECK_EQ(cmp_neg_mask_f64(pos0, neg0), 2 + 4 + 16);
    C17_CHECK_EQ(cmp_mask_f64_mat(ninf, inf),  2 + 4 + 8);
    C17_CHECK_EQ(cmp_mask_f64_mat(inf,  inf),  41);
    C17_CHECK_EQ(cmp_mask_f64_mat(one,  two),  2 + 4 + 8);
    C17_CHECK_EQ(cmp_mask_f64_mat(two,  one),  2 + 16 + 32);

    /* 3. Shared-condition multi-select & FP select on NaN vs ordered */
    {
        /* NaN: lt=0, eq=0, ne=1 -> y1 + 2*y2 + 4*y3 + 8*y2 + 16*x3 */
        int m_nan = multi_select_f64(qnan, one, 1, 10, 2, 20, 3, 30);
        C17_CHECK_EQ(m_nan, 10 + 40 + 120 + 160 + 48);
        /* 1 < 2: lt=1, eq=0, ne=1 -> x1 + 2*x2 + 4*x3 + 8*y2 + 16*x3 */
        int m_lt = multi_select_f64(one, two, 1, 10, 2, 20, 3, 30);
        C17_CHECK_EQ(m_lt, 1 + 4 + 12 + 160 + 48);
        /* 1 == 1: lt=0, eq=1, ne=0 -> y1 + 2*y2 + 4*y3 + 8*x1 + 16*y1 */
        int m_eq = multi_select_f64(one, one, 1, 10, 2, 20, 3, 30);
        C17_CHECK_EQ(m_eq, 10 + 40 + 120 + 8 + 160);

        /* FP select on NaN: (lt?tv:fv)=fv, (eq?tv:fv)=fv, (ne?tv:fv)=tv */
        C17_CHECK_DBL(fp_select_f64(qnan, one, 7.0, 2.0), 2.0 + 20.0 + 700.0, 1e-12);
        C17_CHECK_DBL(fp_select_f64(one, one,  7.0, 2.0), 2.0 + 70.0 + 200.0, 1e-12);
        C17_CHECK_DBL(fp_select_f64(one, two,  7.0, 2.0), 7.0 + 20.0 + 700.0, 1e-12);
    }

    /* 4. uint64_t <-> double/float across 2^63 split */
    {
        uint64_t p63 = UINT64_C(1) << 63;
        /* Exact representable doubles above & below 2^63 */
        uint64_t below = p63 - UINT64_C(2048);
        uint64_t above = p63 + UINT64_C(4096);
        uint64_t near_max = UINT64_MAX - UINT64_C(2047); /* 2^64 - 2048, exact in f64 */

        double d_below = u64_to_f64(opaque_u64(below));
        double d_p63   = u64_to_f64(opaque_u64(p63));
        double d_above = u64_to_f64(opaque_u64(above));
        double d_max   = u64_to_f64(opaque_u64(near_max));

        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(d_below)), below);
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(d_p63)),   p63);
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(d_above)), above);
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(d_max)),   near_max);
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(0.0)),     0ULL);
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(42.9)),    42ULL);

        /* Float32 across 2^63 (2^63 and 2^63 + 2^40 are exact in IEEE-754 float) */
        uint64_t f_above = p63 + (UINT64_C(1) << 40);
        float fl_p63   = u64_to_f32(opaque_u64(p63));
        float fl_above = u64_to_f32(opaque_u64(f_above));
        C17_CHECK_UEQ(f32_to_u64(opaque_f32(fl_p63)),   p63);
        C17_CHECK_UEQ(f32_to_u64(opaque_f32(fl_above)), f_above);

        /* Rounding tie-to-even for odd uint64_t >= 2^63 (LSB sticky bit in uitofp!) */
        uint64_t odd_big = p63 + UINT64_C(1025); /* halfway+1 between p63 and p63+2048 -> rounds UP to p63+2048 */
        double d_odd = u64_to_f64(opaque_u64(odd_big));
        C17_CHECK_UEQ(f64_to_u64(c17_opaque_dbl(d_odd)), p63 + UINT64_C(2048));

        /* uint32_t above 2^31 */
        uint32_t u32_hi = 0xFFFFFFFEU;
        C17_CHECK_UEQ(f64_to_u32(c17_opaque_dbl(u32_to_f64(u32_hi))), u32_hi);
        C17_CHECK_UEQ(f64_to_u32(c17_opaque_dbl(2147483648.0)), 0x80000000U);

        /* Signed int64_t boundary */
        int64_t i_min = opaque_i64(INT64_MIN);
        C17_CHECK_EQ((int64_t)c17_opaque_dbl((double)i_min), INT64_MIN);
    }

    C17_END();
}
