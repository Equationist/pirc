/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.15 / §5.2.4.2.2 — Real-world LLVM/GCC/ARM64
 * NEON fmin/fmax vectorization miscompilation bug:
 *   - `(a < b) ? a : b` and `(a > b) ? a : b` MUST preserve exact IEEE-754
 *     branch semantics for NaN and signed zeros (-0.0 vs +0.0) in BOTH scalar
 *     code AND auto-vectorized loops!
 *     * (NaN < 1.0) ? NaN : 1.0 == 1.0  (NOT NaN! ARM64 `fmin` returns NaN!)
 *     * (1.0 < NaN) ? 1.0 : NaN == NaN  (NOT 1.0! ARM64 `fminnm` returns 1.0!)
 *     * (-0.0 < +0.0) ? -0.0 : +0.0 == +0.0 (NOT -0.0! `fmin` returns -0.0!)
 *     * (+0.0 < -0.0) ? +0.0 : -0.0 == -0.0
 */
#include <math.h>
#include "c17_test.h"

static void vec_min_f64(double *restrict dst, const double *restrict a,
                        const double *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = (a[i] < b[i]) ? a[i] : b[i];
    }
}

static void vec_max_f64(double *restrict dst, const double *restrict a,
                        const double *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = (a[i] > b[i]) ? a[i] : b[i];
    }
}

static void vec_min_f32(float *restrict dst, const float *restrict a,
                        const float *restrict b, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = (a[i] < b[i]) ? a[i] : b[i];
    }
}

int main(void) {
    C17_BEGIN("c65_bug_vec_fminmax_nan_zero",
              "Vectorized (a < b ? a : b) IEEE-754 NaN & signed-zero semantics");

    /* 16-element double arrays (triggers 2x/4x NEON vectorization!) */
    {
        double a[16], b[16], dmin[16], dmax[16];
        for (int i = 0; i < 16; i++) {
            a[i] = (double)i;
            b[i] = (double)(15 - i);
        }
        /* Plant IEEE-754 traps in lanes 0..5 */
        a[0] = (double)NAN;  b[0] = 1.0;          /* min -> 1.0,  max -> 1.0  */
        a[1] = 1.0;          b[1] = (double)NAN;  /* min -> NaN,  max -> NaN  */
        a[2] = -0.0;         b[2] = +0.0;         /* min -> +0.0, max -> +0.0 */
        a[3] = +0.0;         b[3] = -0.0;         /* min -> -0.0, max -> -0.0 */
        a[4] = -5.0;         b[4] = 3.0;          /* min -> -5.0, max -> 3.0  */
        a[5] = 7.0;          b[5] = 2.0;          /* min -> 2.0,  max -> 7.0  */

        int n = c17_opaque_int(16);
        vec_min_f64(dmin, a, b, n);
        vec_max_f64(dmax, a, b, n);

        /* Lane 0: a=NaN, b=1.0 -> cond false -> must pick b (1.0)! */
        C17_CHECK(!isnan(dmin[0]) && dmin[0] == 1.0);
        C17_CHECK(!isnan(dmax[0]) && dmax[0] == 1.0);

        /* Lane 1: a=1.0, b=NaN -> cond false -> must pick b (NaN)! */
        C17_CHECK(isnan(dmin[1]));
        C17_CHECK(isnan(dmax[1]));

        /* Lane 2: a=-0.0, b=+0.0 -> cond false -> must pick b (+0.0, signbit==0)! */
        C17_CHECK(dmin[2] == 0.0 && signbit(dmin[2]) == 0);
        C17_CHECK(dmax[2] == 0.0 && signbit(dmax[2]) == 0);

        /* Lane 3: a=+0.0, b=-0.0 -> cond false -> must pick b (-0.0, signbit!=0)! */
        C17_CHECK(dmin[3] == 0.0 && signbit(dmin[3]) != 0);
        C17_CHECK(dmax[3] == 0.0 && signbit(dmax[3]) != 0);

        /* Normal lanes */
        C17_CHECK_DBL(dmin[4], -5.0, 0.0);
        C17_CHECK_DBL(dmax[4], 3.0, 0.0);
        C17_CHECK_DBL(dmin[5], 2.0, 0.0);
        C17_CHECK_DBL(dmax[5], 7.0, 0.0);
    }

    /* 16-element float arrays (4-wide NEON) */
    {
        float a[16], b[16], out[16];
        for (int i = 0; i < 16; i++) {
            a[i] = (float)i;
            b[i] = (float)(15 - i);
        }
        a[0] = (float)NAN; b[0] = 2.0f;
        a[1] = 2.0f;       b[1] = (float)NAN;
        a[2] = -0.0f;      b[2] = +0.0f;
        a[3] = +0.0f;      b[3] = -0.0f;

        vec_min_f32(out, a, b, c17_opaque_int(16));
        C17_CHECK(!isnan(out[0]) && out[0] == 2.0f);
        C17_CHECK(isnan(out[1]));
        C17_CHECK(out[2] == 0.0f && signbit(out[2]) == 0);
        C17_CHECK(out[3] == 0.0f && signbit(out[3]) != 0);
    }

    C17_END();
}
