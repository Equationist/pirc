/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) Annex F / IEEE-754 — Floating-point signed-zero,
 * NaN, Inf, and algebraic-identity traps in GVN/CSE/select/constant folding.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "c17_test.h"

static uint64_t dbl_bits(double d) {
    uint64_t u = 0;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static uint32_t flt_bits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    return u;
}

static double add_zero(double x) { return x + 0.0; }
static double sub_self(double x) { return x - x; }
static double mul_zero(double x) { return x * 0.0; }
static int eq_self(double x) { return x == x; }
static int ne_self(double x) { return x != x; }
static int not_lt(double a, double b) { return !(a < b); }
static int ge_cmp(double a, double b) { return a >= b; }
static double select_zero(int c) { return c ? -0.0 : 0.0; }

static double (*volatile g_add_zero)(double) = add_zero;
static double (*volatile g_sub_self)(double) = sub_self;
static double (*volatile g_mul_zero)(double) = mul_zero;
static int (*volatile g_eq_self)(double) = eq_self;
static int (*volatile g_ne_self)(double) = ne_self;
static int (*volatile g_not_lt)(double, double) = not_lt;
static int (*volatile g_ge_cmp)(double, double) = ge_cmp;
static double (*volatile g_sel_zero)(int) = select_zero;

int main(void) {
    C17_BEGIN("c65_bug_fp_ieee_negzero_nan_gvn",
              "IEEE-754 -0.0, NaN, Inf algebraic-identity traps");

    double pz = c17_opaque_dbl(0.0);
    double nz = -pz;
    double nan_v = pz / pz;
    double pinf = 1.0 / pz;
    double ninf = -1.0 / pz;

    /* 1. -0.0 signbit preservation and x + 0.0 == +0.0 (MUST NOT fold to x!) */
    C17_CHECK_EQ(dbl_bits(pz), UINT64_C(0x0000000000000000));
    C17_CHECK_EQ(dbl_bits(nz), UINT64_C(0x8000000000000000));
    C17_CHECK(nz == pz);
    C17_CHECK_EQ(dbl_bits(g_add_zero(nz)), UINT64_C(0x0000000000000000));
    C17_CHECK_EQ(dbl_bits(nz + nz), UINT64_C(0x8000000000000000));
    C17_CHECK(1.0 / pz > 0.0 && isinf(1.0 / pz));
    C17_CHECK(1.0 / nz < 0.0 && isinf(1.0 / nz));

    /* 2. select(-0.0, +0.0) must not collapse equal-value branches! */
    C17_CHECK_EQ(dbl_bits(g_sel_zero(c17_opaque_int(1))), UINT64_C(0x8000000000000000));
    C17_CHECK_EQ(dbl_bits(g_sel_zero(c17_opaque_int(0))), UINT64_C(0x0000000000000000));

    /* 3. x - x MUST NOT fold to 0.0 for NaN or Inf! */
    C17_CHECK(isnan(g_sub_self(nan_v)));
    C17_CHECK(isnan(g_sub_self(pinf)));
    C17_CHECK(isnan(g_sub_self(ninf)));
    C17_CHECK_EQ(dbl_bits(g_sub_self(nz)), UINT64_C(0x0000000000000000));

    /* 4. x * 0.0 MUST NOT fold to +0.0 for NaN, Inf, or negative numbers! */
    C17_CHECK(isnan(g_mul_zero(nan_v)));
    C17_CHECK(isnan(g_mul_zero(pinf)));
    C17_CHECK(isnan(g_mul_zero(ninf)));
    C17_CHECK_EQ(dbl_bits(g_mul_zero(c17_opaque_dbl(-5.0))), UINT64_C(0x8000000000000000));
    C17_CHECK_EQ(dbl_bits(g_mul_zero(c17_opaque_dbl(5.0))),  UINT64_C(0x0000000000000000));

    /* 5. x == x / x != x / !(a < b) vs (a >= b) on NaN */
    C17_CHECK_EQ(g_eq_self(nan_v), 0);
    C17_CHECK_EQ(g_ne_self(nan_v), 1);
    C17_CHECK_EQ(g_eq_self(nz), 1);
    C17_CHECK_EQ(g_not_lt(nan_v, 1.0), 1);
    C17_CHECK_EQ(g_ge_cmp(nan_v, 1.0), 0);
    C17_CHECK_EQ(g_not_lt(1.0, nan_v), 1);
    C17_CHECK_EQ(g_ge_cmp(1.0, nan_v), 0);

    /* 6. float <-> double conversions preserve -0.0f and NaN */
    {
        float fnz = (float)nz;
        C17_CHECK_EQ(flt_bits(fnz), UINT32_C(0x80000000));
        double back = (double)fnz;
        C17_CHECK_EQ(dbl_bits(back), UINT64_C(0x8000000000000000));
        float fnan = (float)nan_v;
        C17_CHECK(isnan(fnan));
    }

    C17_END();
}
