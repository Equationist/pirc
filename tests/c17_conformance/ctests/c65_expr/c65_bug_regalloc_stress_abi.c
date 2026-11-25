/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Real-world register allocator & ABI copy-cycle bugs:
 *   - Argument-reversal call callee(h, g, f, e, d, c, b, a) creates 4 simultaneous
 *     2-cycles in ABI argument registers (x0<->x7, x1<->x6, x2<->x5, x3<->x4)
 *   - 16 live GP + 16 live FP values surviving across multiple calls (spilling both
 *     GP and FP callee-saved/stack slots simultaneously)
 *   - 5-way permutation cycle inside a loop containing a call
 */
#include <stdint.h>
#include "c17_test.h"

static int64_t sink8(int64_t a, int64_t b, int64_t c, int64_t d,
                     int64_t e, int64_t f, int64_t g, int64_t h) {
    volatile int64_t v = a * 1 + b * 2 + c * 3 + d * 4 +
                         e * 5 + f * 6 + g * 7 + h * 8;
    return v;
}

/* Reversing all 8 incoming GP registers at a call site stresses parallel-copy resolution */
static int64_t reverse_call_gp(int64_t a, int64_t b, int64_t c, int64_t d,
                               int64_t e, int64_t f, int64_t g, int64_t h) {
    return sink8(h, g, f, e, d, c, b, a);
}

static double sink8_fp(double a, double b, double c, double d,
                       double e, double f, double g, double h) {
    volatile double v = a * 1.0 + b * 2.0 + c * 3.0 + d * 4.0 +
                        e * 5.0 + f * 6.0 + g * 7.0 + h * 8.0;
    return v;
}

static double reverse_call_fp(double a, double b, double c, double d,
                              double e, double f, double g, double h) {
    return sink8_fp(h, g, f, e, d, c, b, a);
}

static int64_t bump_call(int64_t x) {
    volatile int64_t v = x;
    return v + 1;
}

/* 16 live GP + 16 live FP variables across two calls */
static int64_t reg_pressure_gp_fp(int64_t s, double ds) {
    int64_t g0 = s + 1, g1 = s + 2, g2 = s + 3, g3 = s + 4;
    int64_t g4 = s + 5, g5 = s + 6, g6 = s + 7, g7 = s + 8;
    int64_t g8 = s + 9, g9 = s + 10, g10 = s + 11, g11 = s + 12;
    int64_t g12 = s + 13, g13 = s + 14, g14 = s + 15, g15 = s + 16;

    double f0 = ds + 1.0, f1 = ds + 2.0, f2 = ds + 3.0, f3 = ds + 4.0;
    double f4 = ds + 5.0, f5 = ds + 6.0, f6 = ds + 7.0, f7 = ds + 8.0;
    double f8 = ds + 9.0, f9 = ds + 10.0, f10 = ds + 11.0, f11 = ds + 12.0;
    double f12 = ds + 13.0, f13 = ds + 14.0, f14 = ds + 15.0, f15 = ds + 16.0;

    int64_t c1 = reverse_call_gp(g0, g1, g2, g3, g4, g5, g6, g7);
    double c2 = reverse_call_fp(f0, f1, f2, f3, f4, f5, f6, f7);

    int64_t gsum = g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7 +
                   g8 + g9 + g10 + g11 + g12 + g13 + g14 + g15;
    double fsum = f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 +
                  f8 + f9 + f10 + f11 + f12 + f13 + f14 + f15;
    return c1 + (int64_t)c2 + gsum + (int64_t)fsum;
}

/* 5-way register permutation across loop backedge with an internal call */
static int64_t permute5_loop(int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int n) {
    for (int i = 0; i < n; i++) {
        int64_t na = b;
        int64_t nb = c;
        int64_t nc = d;
        int64_t nd = e;
        int64_t ne = bump_call(a);
        a = na; b = nb; c = nc; d = nd; e = ne;
    }
    return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
}

int main(void) {
    C17_BEGIN("c65_bug_regalloc_stress_abi",
              "Regalloc ABI reverse-arg copy cycles, 16GP+16FP spills, 5-way phi cycle");

    /* reverse_call_gp(1..8) calls sink8(8,7,6,5,4,3,2,1):
     * 8*1 + 7*2 + 6*3 + 5*4 + 4*5 + 3*6 + 2*7 + 1*8 = 120 */
    C17_CHECK_EQ(reverse_call_gp(1, 2, 3, 4, 5, 6, 7, 8), 120);
    C17_CHECK_DBL(reverse_call_fp(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0), 120.0, 0.0);

    /* reg_pressure_gp_fp(0, 0.0):
     * c1 = 120, c2 = 120, gsum = 136, fsum = 136 -> total = 512 */
    C17_CHECK_EQ(reg_pressure_gp_fp(c17_opaque_ll(0), c17_opaque_dbl(0.0)), 512);

    /* permute5_loop(1,2,3,4,5, 5): each element rotates once full circle and gets +1 -> (2,3,4,5,6) */
    C17_CHECK_EQ(permute5_loop(c17_opaque_ll(1), c17_opaque_ll(2), c17_opaque_ll(3),
                               c17_opaque_ll(4), c17_opaque_ll(5), c17_opaque_int(5)), 23456);
    /* 7 steps: (3,4,5,6,7) after 5 -> step6:(4,5,6,7,3+1? wait: (2,3,4,5,6)->(3,4,5,6,3)->(4,5,6,3,4)) */
    C17_CHECK_EQ(permute5_loop(c17_opaque_ll(1), c17_opaque_ll(2), c17_opaque_ll(3),
                               c17_opaque_ll(4), c17_opaque_ll(5), c17_opaque_int(7)), 45634);

    C17_END();
}
