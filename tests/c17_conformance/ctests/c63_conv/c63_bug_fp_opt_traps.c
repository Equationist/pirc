/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §5.2.4.2.2 — Real-world FP algebraic-folding bugs
 * (GCC, LLVM, QBE, CompCert):
 *   - x - x MUST NOT fold to 0.0 (Inf - Inf == NaN, NaN - NaN == NaN!)
 *   - x == x MUST NOT fold to 1 (NaN == NaN is false!)
 *   - x != x MUST NOT fold to 0 (NaN != NaN is true!)
 *   - x / 1.0 preserves -0.0; 0.0 / -1.0 produces -0.0
 *   - Subnormal (denormal) arithmetic must NOT flush to zero
 */
#include <float.h>
#include <math.h>
#include "c17_test.h"

static double self_sub(double x) { return x - x; }
static int self_eq(double x) { return x == x; }
static int self_ne(double x) { return x != x; }
static double div_one(double x) { return x / 1.0; }
static double div_neg_one(double x) { return x / -1.0; }

int main(void) {
    C17_BEGIN("c63_bug_fp_opt_traps",
              "x-x on Inf/NaN, x==x/x!=x on NaN, signed zero div, subnormals");

    /* x - x must NOT be algebraically folded to 0.0 */
    {
        double inf = c17_opaque_dbl((double)INFINITY);
        double ninf = c17_opaque_dbl(-(double)INFINITY);
        double qnan = c17_opaque_dbl((double)NAN);
        double reg = c17_opaque_dbl(42.5);
        C17_CHECK(isnan(self_sub(inf)));
        C17_CHECK(isnan(self_sub(ninf)));
        C17_CHECK(isnan(self_sub(qnan)));
        C17_CHECK_DBL(self_sub(reg), 0.0, 0.0);
        C17_CHECK(signbit(self_sub(c17_opaque_dbl(-0.0))) == 0); /* -0.0 - -0.0 == +0.0 */
    }

    /* x == x and x != x must NOT be folded to constants */
    {
        double qnan = c17_opaque_dbl((double)NAN);
        double x = c17_opaque_dbl(1.5);
        C17_CHECK_EQ(self_eq(qnan), 0);
        C17_CHECK_EQ(self_ne(qnan), 1);
        C17_CHECK_EQ(self_eq(x), 1);
        C17_CHECK_EQ(self_ne(x), 0);
    }

    /* Signed zero under / 1.0 and / -1.0 */
    {
        double pz = c17_opaque_dbl(0.0);
        double nz = c17_opaque_dbl(-0.0);
        C17_CHECK(signbit(div_one(nz)) != 0);
        C17_CHECK(signbit(div_one(pz)) == 0);
        C17_CHECK(signbit(div_neg_one(pz)) != 0);
        C17_CHECK(signbit(div_neg_one(nz)) == 0);
    }

    /* Subnormal (denormal) arithmetic must NOT flush to zero */
    {
        double dmin = c17_opaque_dbl(DBL_MIN);
        double sub = dmin / 2.0;
        C17_CHECK(sub > 0.0);
        C17_CHECK(sub != 0.0);
        C17_CHECK(sub * 2.0 == dmin);

        float fmin = c17_opaque_flt(FLT_MIN);
        float fsub = fmin / 2.0f;
        C17_CHECK(fsub > 0.0f);
        C17_CHECK(fsub * 2.0f == fmin);
    }

    C17_END();
}
