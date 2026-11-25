/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Tail-call optimization & accumulator-TCO
 * edge cases (stressing simplify_cfg.c self-tail & acc-tail lowering):
 *   - Direct self-tail recursion rotating 6 parameters
 *   - Accumulator TCO with ADD (`n + sum(n-1)`) and MUL (`n * fact(n-1)`)
 *   - Mixed non-tail `100 - f(n-1)` + acc-tail `2 * f(n-1)`
 *   - Double recursion `fib(n-1) + fib(n-2)`
 *   - Recursion reading a global modified by base case AFTER recursive call
 */
#include <stdint.h>
#include "c17_test.h"

static int64_t tco_rot6(int n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    if (n <= 0) {
        return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
    }
    return tco_rot6(n - 1, b, c, d, e, a);
}

static int64_t tco_sum_acc(int64_t n) {
    if (n <= 0) return 0;
    return n + tco_sum_acc(n - 1);
}

static int64_t tco_fact_acc(int64_t n) {
    if (n <= 1) return 1;
    return n * tco_fact_acc(n - 1);
}

static int tco_mixed_sub_mul(int n) {
    if (n <= 0) return 5;
    if (n == 2) return 100 - tco_mixed_sub_mul(n - 1);
    return 2 * tco_mixed_sub_mul(n - 1);
}

static int fib_rec(int n) {
    if (n <= 1) return n;
    return fib_rec(n - 1) + fib_rec(n - 2);
}

static int g_after = 0;

static int rec_post_global(int n) {
    if (n <= 0) {
        g_after = 100;
        return 1;
    }
    int r = rec_post_global(n - 1);
    /* g_after is now 100 (modified in base case!), NOT its pre-call value 0! */
    return r + g_after + n;
}

int main(void) {
    C17_BEGIN("c65_bug_tco_and_recursion_edge",
              "6-param rotating TCO, ADD/MUL acc-TCO, mixed sub/mul, fib, post-call global");

    /* 5-element rotation: 1 step -> (2,3,4,5,1), 5 steps -> (1,2,3,4,5), 7 steps -> (3,4,5,1,2) */
    C17_CHECK_EQ(tco_rot6(c17_opaque_int(1), 1, 2, 3, 4, 5), 23451);
    C17_CHECK_EQ(tco_rot6(c17_opaque_int(5), 1, 2, 3, 4, 5), 12345);
    C17_CHECK_EQ(tco_rot6(c17_opaque_int(7), 1, 2, 3, 4, 5), 34512);

    /* ADD & MUL accumulator TCO */
    C17_CHECK_EQ(tco_sum_acc(c17_opaque_ll(100)), 5050);
    C17_CHECK_EQ(tco_fact_acc(c17_opaque_ll(12)), 479001600LL);

    /* Mixed 100 - f(n-1) and 2 * f(n-1):
     * f(0)=5, f(1)=10, f(2)=90, f(3)=180, f(4)=360 */
    C17_CHECK_EQ(tco_mixed_sub_mul(c17_opaque_int(3)), 180);
    C17_CHECK_EQ(tco_mixed_sub_mul(c17_opaque_int(4)), 360);

    /* Double recursion */
    C17_CHECK_EQ(fib_rec(c17_opaque_int(10)), 55);

    /* Global modified in base case observed after recursive call:
     * n=0 -> g_after=100, ret 1
     * n=1 -> 1 + 100 + 1 = 102
     * n=2 -> 102 + 100 + 2 = 204
     * n=3 -> 204 + 100 + 3 = 307
     */
    g_after = 0;
    C17_CHECK_EQ(rec_post_global(c17_opaque_int(3)), 307);

    C17_END();
}
