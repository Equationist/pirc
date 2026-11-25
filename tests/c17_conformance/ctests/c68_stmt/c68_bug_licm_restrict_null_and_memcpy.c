/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.3.1 / §6.8.5 — Real-world LICM safety regressions:
 *   - `int *restrict p` CAN legally be NULL! LICM must NOT hoist `*p` out of
 *     `if (p != NULL)` into the loop preheader (would SIGSEGV!).
 *   - Out-of-bounds global array index guarded by a false condition must NOT
 *     be hoisted out of the loop!
 *   - Loop containing struct memcpy/memset modifying a struct field must NOT
 *     hoist loads of that field across iterations!
 */
#include <stddef.h>
#include <string.h>
#include "c17_test.h"

/* `restrict` parameter legally passed NULL! Hoisting *p out of `if (p != NULL)` crashes! */
static int licm_restrict_null_guard(int *restrict p, int n) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (p != NULL) {
            sum += *p;
        } else {
            sum += 2;
        }
    }
    return sum;
}

/* `restrict` pointer with out-of-bounds offset guarded by `flag == 0` */
static int licm_restrict_oob_guard(int *restrict p, int n, int flag) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (flag) {
            sum += p[1000000];
        } else {
            sum += 3;
        }
    }
    return sum;
}

static int g_tiny[2] = { 11, 22 };

/* Global array with out-of-bounds index guarded by `flag == 0` */
static int licm_global_oob_guard(int n, int flag) {
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (flag) {
            sum += g_tiny[2000000];
        } else {
            sum += g_tiny[0];
        }
    }
    return sum;
}

struct box {
    int a;
    int b;
    int c;
    int d;
};

/* Loop modifying `dst` via memcpy/memset must NOT allow LICM to hoist `dst.a`! */
static int licm_memcpy_clobber(int n) {
    struct box dst = { 1, 2, 3, 4 };
    struct box src = { 10, 20, 30, 40 };
    int sum = 0;
    for (int i = 0; i < n; i++) {
        sum += dst.a;
        if (i == 1) {
            memcpy(&dst, &src, sizeof(dst));
        } else if (i == 3) {
            memset(&dst, 0, sizeof(dst));
        }
    }
    return sum;
}

int main(void) {
    C17_BEGIN("c68_bug_licm_restrict_null_and_memcpy",
              "LICM restrict NULL/OOB hoisting, global OOB hoisting, memcpy/memset clobber");

    /* Prevent inlining via volatile function pointers so LICM optimizes the
     * standalone loop bodies with real restrict/global parameters! */
    int (*volatile fn_rnull)(int *restrict, int) = licm_restrict_null_guard;
    int (*volatile fn_roob)(int *restrict, int, int) = licm_restrict_oob_guard;
    int (*volatile fn_goob)(int, int) = licm_global_oob_guard;
    int (*volatile fn_mc)(int) = licm_memcpy_clobber;

    /* Calling restrict fn with NULL: MUST NOT SIGSEGV! */
    C17_CHECK_EQ(fn_rnull(NULL, c17_opaque_int(5)), 10);
    C17_CHECK_EQ(fn_rnull(NULL, c17_opaque_int(0)), 0);
    {
        int x = c17_opaque_int(7);
        C17_CHECK_EQ(fn_rnull(&x, c17_opaque_int(4)), 28);
        C17_CHECK_EQ(fn_roob(&x, c17_opaque_int(4), c17_opaque_int(0)), 12);
    }

    /* Guarded out-of-bounds global access: MUST NOT SIGSEGV! */
    C17_CHECK_EQ(fn_goob(c17_opaque_int(4), c17_opaque_int(0)), 44);

    /* memcpy/memset inside loop:
     * i=0: sum+=1 (1)
     * i=1: sum+=1 (2), memcpy -> dst.a=10
     * i=2: sum+=10 (12)
     * i=3: sum+=10 (22), memset -> dst.a=0
     * i=4: sum+=0 (22)
     */
    C17_CHECK_EQ(fn_mc(c17_opaque_int(5)), 22);

    C17_END();
}
