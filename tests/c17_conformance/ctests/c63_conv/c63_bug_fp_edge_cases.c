/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §5.2.4.2.2 / §6.5.8 / §6.3.1.4 — Real-world FP bugs:
 *   - Signed zero (-0.0 vs +0.0): x + 0.0 != x when x is -0.0; x * 0.0 != +0.0
 *     when x is negative or NaN/Inf
 *   - NaN unordered comparisons & ternary branch-inversion bugs
 *   - float vs double precision preservation
 *   - int64_t <-> double/float exact boundary conversions
 */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include "c17_test.h"

static double add_zero(double x) { return x + 0.0; }
static double mul_zero(double x) { return x * 0.0; }
static int lt_branch(double a, double b) { return (a < b) ? 1 : 2; }
static int le_branch(double a, double b) { return (a <= b) ? 1 : 2; }
static int gt_branch(double a, double b) { return (a > b) ? 1 : 2; }
static int ge_branch(double a, double b) { return (a >= b) ? 1 : 2; }
static int eq_branch(double a, double b) { return (a == b) ? 1 : 2; }
static int ne_branch(double a, double b) { return (a != b) ? 1 : 2; }

int main(void) {
    C17_BEGIN("c63_bug_fp_edge_cases",
              "IEEE-754 signed zero, NaN branch inversion, float precision, i64<->fp");

    /* Signed zero arithmetic: optimizers must NOT fold x + 0.0 -> x or x * 0.0 -> 0.0 */
    {
        double nz = c17_opaque_dbl(-0.0);
        double pz = c17_opaque_dbl(0.0);
        C17_CHECK(nz == pz);
        C17_CHECK(signbit(nz) != 0);
        C17_CHECK(signbit(pz) == 0);

        /* (-0.0) + (+0.0) MUST be +0.0 (round-to-nearest-even) */
        double sum1 = add_zero(nz);
        C17_CHECK(sum1 == 0.0);
        C17_CHECK(signbit(sum1) == 0);

        /* (-0.0) + (-0.0) MUST be -0.0 */
        double sum2 = nz + c17_opaque_dbl(-0.0);
        C17_CHECK(sum2 == 0.0);
        C17_CHECK(signbit(sum2) != 0);

        /* (-5.0) * 0.0 MUST be -0.0 */
        double prod_neg = mul_zero(c17_opaque_dbl(-5.0));
        C17_CHECK(prod_neg == 0.0);
        C17_CHECK(signbit(prod_neg) != 0);

        /* Inf * 0.0 MUST be NaN */
        double prod_inf = mul_zero(c17_opaque_dbl((double)INFINITY));
        C17_CHECK(isnan(prod_inf));

        /* 1.0 / -0.0 == -Inf */
        double inv = 1.0 / nz;
        C17_CHECK(isinf(inv) && inv < 0.0);
    }

    /* NaN unordered comparisons & branch inversion (fcmp olt vs uge bug) */
    {
        double qnan = c17_opaque_dbl((double)NAN);
        double one = c17_opaque_dbl(1.0);
        C17_CHECK_EQ(lt_branch(qnan, one), 2);
        C17_CHECK_EQ(le_branch(qnan, one), 2);
        C17_CHECK_EQ(gt_branch(qnan, one), 2);
        C17_CHECK_EQ(ge_branch(qnan, one), 2);
        C17_CHECK_EQ(eq_branch(qnan, qnan), 2);
        C17_CHECK_EQ(ne_branch(qnan, qnan), 1);

        C17_CHECK_EQ(lt_branch(one, qnan), 2);
        C17_CHECK_EQ(le_branch(one, qnan), 2);
        C17_CHECK_EQ(gt_branch(one, qnan), 2);
        C17_CHECK_EQ(ge_branch(one, qnan), 2);
        C17_CHECK_EQ(eq_branch(one, qnan), 2);
        C17_CHECK_EQ(ne_branch(one, qnan), 1);
    }

    /* float vs double precision preservation */
    {
        float f1 = c17_opaque_flt(1.0f);
        float feps = c17_opaque_flt(1e-8f);
        C17_CHECK((f1 + feps) == 1.0f);

        double d1 = c17_opaque_dbl(1.0);
        double deps = c17_opaque_dbl(1e-8);
        C17_CHECK((d1 + deps) != 1.0);
    }

    /* int64_t <-> double exact boundary values */
    {
        int64_t lo = c17_opaque_ll(INT64_MIN);
        double d_lo = (double)lo;
        C17_CHECK_DBL(d_lo, -9223372036854775808.0, 0.0);
        C17_CHECK_EQ((int64_t)d_lo, INT64_MIN);

        int64_t p53 = c17_opaque_ll((1LL << 53) - 1);
        double d_p53 = (double)p53;
        C17_CHECK_EQ((int64_t)d_p53, (1LL << 53) - 1);

        int64_t n53 = c17_opaque_ll(-((1LL << 53) - 1));
        double d_n53 = (double)n53;
        C17_CHECK_EQ((int64_t)d_n53, -((1LL << 53) - 1));
    }

    C17_END();
}
