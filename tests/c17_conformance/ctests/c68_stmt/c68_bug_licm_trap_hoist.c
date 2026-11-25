/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.5 — Real-world LICM bug regressions:
 *   - GCC PR36181 / LLVM PR16756: LICM must NOT hoist trapping operations
 *     (division/modulo by zero, NULL pointer dereference) out of a conditional
 *     branch or a 0-trip loop into the preheader
 *   - LICM must NOT hoist a load whose target is modified by an aliasing call
 *     or pointer inside the loop
 */
#include <stddef.h>
#include "c17_test.h"

/* Must NOT trap when d == 0 (if guard is false) or n == 0 (0-trip loop)! */
static int licm_div_guard(int n, int x, int d, int flag) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        if (flag) {
            acc += x / d + x % d;
        } else {
            acc += i;
        }
    }
    return acc;
}

/* Must NOT segfault when p == NULL and flag == 0 (or n == 0)! */
static int licm_null_guard(int n, const int *p, int flag) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        if (flag && p != NULL) {
            acc += *p;
        } else {
            acc += 3;
        }
    }
    return acc;
}

static int g_shared = 1;

static void maybe_bump_global(int step) {
    volatile int s = step;
    if ((s & 1) == 1) {
        g_shared += 10;
    }
}

/* LICM must reload g_shared across maybe_bump_global() call */
static int licm_call_clobber(int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        acc += g_shared;
        maybe_bump_global(i);
    }
    return acc;
}

int main(void) {
    C17_BEGIN("c68_bug_licm_trap_hoist",
              "LICM trap hoisting (div0, null deref, 0-trip) & call clobber");

    /* d == 0 with flag == 0: would SIGFPE if LICM hoisted x / d! */
    C17_CHECK_EQ(licm_div_guard(c17_opaque_int(10), c17_opaque_int(100),
                                c17_opaque_int(0), c17_opaque_int(0)), 45);
    /* d == 0 with flag == 1, n == 0 (0-trip loop!): must NOT execute division! */
    C17_CHECK_EQ(licm_div_guard(c17_opaque_int(0), c17_opaque_int(100),
                                c17_opaque_int(0), c17_opaque_int(1)), 0);
    /* Valid division path */
    C17_CHECK_EQ(licm_div_guard(c17_opaque_int(4), c17_opaque_int(23),
                                c17_opaque_int(5), c17_opaque_int(1)), 4 * (4 + 3));

    /* NULL pointer with flag == 0 or n == 0: would SIGSEGV if LICM hoisted *p! */
    C17_CHECK_EQ(licm_null_guard(c17_opaque_int(5), NULL, c17_opaque_int(0)), 15);
    C17_CHECK_EQ(licm_null_guard(c17_opaque_int(5), NULL, c17_opaque_int(1)), 15);
    C17_CHECK_EQ(licm_null_guard(c17_opaque_int(0), NULL, c17_opaque_int(1)), 0);
    {
        int val = c17_opaque_int(17);
        C17_CHECK_EQ(licm_null_guard(c17_opaque_int(3), &val, c17_opaque_int(1)), 51);
    }

    /* Global modified inside loop by callee:
     * i=0: acc+=1 (1), bump(0) no-op
     * i=1: acc+=1 (2), bump(1) -> g=11
     * i=2: acc+=11 (13), bump(2) no-op
     * i=3: acc+=11 (24), bump(3) -> g=21
     * i=4: acc+=21 (45), bump(4) no-op
     */
    g_shared = 1;
    C17_CHECK_EQ(licm_call_clobber(c17_opaque_int(5)), 45);
    C17_CHECK_EQ(g_shared, 21);

    C17_END();
}
