/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 p6 / §6.5.2.5 / §6.5.8 — Real-world CompCert /
 * Csmith / GCC / LLVM aliasing & lifetime regressions:
 *   - malloc effective-type re-typing (int -> double -> memcpy)
 *   - Forward & reverse pointer-iteration loops using one-past-the-end &arr[N]
 *   - Compound literal re-initialization & address escape inside a loop
 *   - Union active-member switching inside a loop (GVN/DSE trap)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "c17_test.h"

struct pt {
    int x;
    int y;
};

static int read_pt(const struct pt *p) {
    volatile int vx = p->x;
    volatile int vy = p->y;
    return vx * 10 + vy;
}

union int_or_dbl {
    int64_t i;
    double d;
};

static double union_switch_loop(int n) {
    union int_or_dbl u;
    double acc = 0.0;
    for (int k = 1; k <= n; k++) {
        u.i = (int64_t)k * 100LL;
        int64_t iv = u.i;
        u.d = (double)iv * 0.5;
        acc += u.d;
        u.i = iv + 1;
        acc += (double)(u.i - iv); /* +1.0 */
    }
    return acc;
}

int main(void) {
    C17_BEGIN("c65_bug_compcert_csmith_aliasing",
              "malloc re-typing, one-past-end ptr loops, loop complit, union switch");

    /* C17 §6.5 p6: malloc storage effective type changes on each typed store */
    {
        void *mem = malloc(64);
        C17_CHECK(mem != NULL);
        if (mem) {
            int *ip = (int *)mem;
            ip[0] = c17_opaque_int(12345);
            ip[1] = c17_opaque_int(67890);
            C17_CHECK_EQ(ip[0] + ip[1], 80235);

            /* Re-type same allocated storage as double */
            double *dp = (double *)mem;
            dp[0] = c17_opaque_dbl(3.25);
            dp[1] = c17_opaque_dbl(4.75);
            double copy = 0.0;
            memcpy(&copy, dp, sizeof(double));
            C17_CHECK_DBL(copy + dp[1], 8.0, 0.0);
            free(mem);
        }
    }

    /* Forward & reverse pointer-iteration loops with &arr[N] */
    {
        struct pt arr[6] = {
            { 1, 2 }, { 3, 4 }, { 5, 6 }, { 7, 8 }, { 9, 10 }, { 11, 12 }
        };
        int fsum = 0;
        for (struct pt *p = arr; p != arr + 6; p++) {
            fsum += p->x + p->y;
        }
        int rsum = 0;
        for (struct pt *p = arr + 6; p > arr; ) {
            --p;
            rsum += p->x + p->y;
        }
        /* 1+2+...+12 = 78 */
        C17_CHECK_EQ(fsum, 78);
        C17_CHECK_EQ(rsum, 78);
    }

    /* Compound literal inside loop MUST re-initialize on every iteration (C17 §6.5.2.5 p5) */
    {
        int sum = 0;
        for (int i = 1; i <= 5; i++) {
            struct pt *p = &(struct pt){ i, i * 2 };
            sum += read_pt(p);
            /* Mutating the compound literal must NOT carry over to next iteration */
            p->x = 9999;
            p->y = 9999;
        }
        /* sum of (10*i + 2*i) for i=1..5 = 12 * 15 = 180 */
        C17_CHECK_EQ(sum, 180);
    }

    /* Union active-member switching in loop:
     * k=1..4: sum(50*k + 1) = 50*10 + 4 = 504.0 */
    C17_CHECK_DBL(union_switch_loop(c17_opaque_int(4)), 504.0, 0.0);

    C17_END();
}
