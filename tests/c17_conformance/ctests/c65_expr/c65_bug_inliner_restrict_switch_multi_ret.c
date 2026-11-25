/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.3.1, §6.7.6.2, §6.8.4.2 — Adversarial inliner
 * (inline.c) stress tests:
 *  - Static helper with switch + multiple returns + restrict pointers called
 *    repeatedly with overlapping buffers across calls (disjoint within each call)
 *  - Static helper with static local + local struct alloca called inside a loop
 *  - Helper containing a VLA called 4000 times in a loop (must NOT leak stack!)
 *  - Tiny 3-arg recursive Tak function (exercises try_self_inline_tiny!)
 */
#include <stdint.h>
#include "c17_test.h"

static int g_data[16] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

/* Small static helper with switch, multiple returns, and restrict params */
static int switch_restrict_step(int *restrict dst, const int *restrict src, int mode) {
    switch (mode) {
    case 0:
        *dst = *src + 1;
        return *dst;
    case 1:
        *dst = *src * 2;
        return *dst + 10;
    case 2:
        *dst = *src - 3;
        return *dst - 5;
    default:
        *dst = 0;
        return -1;
    }
}

/* Helper with static local + local struct (hoisted alloca must re-init each call) */
static int static_local_and_struct(int x) {
    static int counter = 0;
    struct { int a; int b; } local = { x, x + 1 };
    counter++;
    return counter + local.a + local.b;
}

/* Helper containing a VLA: MUST NOT be inlined without stack reclamation! */
static int vla_leaf(int n, int seed) {
    int tmp[n];
    for (int i = 0; i < n; i++) tmp[i] = seed + i;
    return tmp[0] + tmp[n - 1];
}

/* Classic 3-argument Tak function: exercises try_self_inline_tiny in inline.c! */
static int tak(int x, int y, int z) {
    if (y < x) {
        return tak(tak(x - 1, y, z), tak(y - 1, z, x), tak(z - 1, x, y));
    }
    return z;
}

static int run_inliner_suite(int mode_bias) {
    /* Call switch_restrict_step on overlapping slices across calls (disjoint per call!) */
    int buf[4] = { 10, 20, 30, 40 };
    int r0 = switch_restrict_step(&buf[1], &buf[0], mode_bias + 0); /* buf[1]=11, r0=11 */
    int r1 = switch_restrict_step(&buf[2], &buf[1], mode_bias + 1); /* buf[2]=22, r1=32 */
    int r2 = switch_restrict_step(&buf[3], &buf[2], mode_bias + 2); /* buf[3]=19, r2=14 */
    int r3 = switch_restrict_step(&buf[0], &g_data[5], mode_bias + 0); /* g_data[5]=6 => buf[0]=7, r3=7 */
    return r0 + r1 + r2 + r3 + buf[0] + buf[1] + buf[2] + buf[3];
}

static int (*volatile g_run_inl)(int) = run_inliner_suite;
static int (*volatile g_tak)(int, int, int) = tak;

int main(void) {
    C17_BEGIN("c65_bug_inliner_restrict_switch_multi_ret",
              "Inliner switch/multi-ret/restrict, static local, VLA loop, Tak self-inline");

    /* 1. switch + multi-ret + restrict across calls:
     * r0=11, r1=32, r2=14, r3=7 => 64; buf={7,11,22,19} => 59; total = 123 */
    C17_CHECK_EQ(g_run_inl(c17_opaque_int(0)), 123);

    /* 2. Static local + local struct inside loop */
    {
        int sum = 0;
        for (int i = 1; i <= 5; i++) {
            sum += static_local_and_struct(i);
        }
        /* counter: 1+2+3+4+5=15; a+b = 2*i+1: 3+5+7+9+11=35; total = 50 */
        C17_CHECK_EQ(sum, 50);
    }

    /* 3. VLA helper called 4000 times (4000 * 4KB = 16MB would overflow 8MB stack if leaked!) */
    {
        int acc = 0;
        int n = c17_opaque_int(1024);
        for (int i = 0; i < 4000; i++) {
            acc += vla_leaf(n, i & 7);
        }
        /* Each call: (i&7) + (i&7) + 1023; avg(i&7) = 3.5 -> 500*(0+1+..+7)*2 + 4000*1023
         * = 500*28*2 + 4092000 = 28000 + 4092000 = 4120000 */
        C17_CHECK_EQ(acc, 4120000);
    }

    /* 4. 3-arg Tak recursion (self-inlining stress!) */
    C17_CHECK_EQ(g_tak(c17_opaque_int(12), c17_opaque_int(6), c17_opaque_int(0)), 1);
    C17_CHECK_EQ(g_tak(c17_opaque_int(18), c17_opaque_int(12), c17_opaque_int(6)), 7);

    C17_END();
}
