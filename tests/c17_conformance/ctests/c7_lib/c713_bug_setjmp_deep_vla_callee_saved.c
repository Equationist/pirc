/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.13 — Deep multi-frame setjmp/longjmp unwinding
 * across nested VLA frames, >4KB stack frames, and callee-saved GP+FP state,
 * verifying volatile locals and callee-saved registers survive longjmp intact.
 */
#include <setjmp.h>
#include <stdint.h>
#include "c17_test.h"

static jmp_buf g_jb;
static volatile int g_sink = 0;

static void deep_vla_jump(int depth, int n, int val) {
    int vla[n];
    unsigned char pad[5000];
    for (int i = 0; i < n; i++) vla[i] = depth + i;
    pad[0] = (unsigned char)depth;
    pad[4999] = (unsigned char)val;
    g_sink += vla[n - 1] + pad[0] + pad[4999];
    if (depth <= 0) {
        longjmp(g_jb, val);
    }
    deep_vla_jump(depth - 1, n + 3, val);
}

static void (*volatile g_deep_jmp)(int, int, int) = deep_vla_jump;

static int run_setjmp_frame(int seed) {
    /* Non-volatile variables NOT modified after setjmp MUST survive longjmp! */
    int k0 = seed + 1, k1 = seed + 2, k2 = seed + 3, k3 = seed + 4;
    int k4 = seed + 5, k5 = seed + 6, k6 = seed + 7, k7 = seed + 8;
    double d0 = (double)seed + 0.5, d1 = (double)seed + 1.5;
    double d2 = (double)seed + 2.5, d3 = (double)seed + 3.5;

    /* Volatile local modified across setjmp/longjmp MUST reflect latest store */
    volatile int step = 0;

    int rc = setjmp(g_jb);
    if (rc == 0) {
        step = 10;
        g_deep_jmp(4, 17, 77);
        return -999; /* unreachable */
    }
    if (rc == 77) {
        step += 5;
        g_deep_jmp(3, 11, 88);
        return -998; /* unreachable */
    }
    /* Second longjmp landed with rc == 88 and step == 15 */
    int sum = k0 + k1 + k2 + k3 + k4 + k5 + k6 + k7;
    sum += (int)(d0 + d1 + d2 + d3);
    return sum + step + rc;
}

static int (*volatile g_run_sj)(int) = run_setjmp_frame;

int main(void) {
    C17_BEGIN("c713_bug_setjmp_deep_vla_callee_saved",
              "Deep multi-frame setjmp/longjmp across VLAs, >4KB frames & GP+FP regs");

    /* seed=10:
     * k0..k7 = 11..18 => sum = 116
     * d0..d3 = 10.5+11.5+12.5+13.5 = 48
     * step = 15, rc = 88 => total = 116 + 48 + 15 + 88 = 267 */
    C17_CHECK_EQ(g_run_sj(c17_opaque_int(10)), 267);

    C17_END();
}
