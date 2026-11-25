/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.5, §6.7.6.2 — Over-aligned VLAs, over-aligned
 * local scalars/arrays combined with VLAs, >16KB stack frames (chkstk),
 * high register pressure (callee-saved spills), variadic calls, and nested
 * VLA stackrestore via goto.
 */
#include <stdarg.h>
#include <stdint.h>
#include "c17_test.h"

struct over64 {
    _Alignas(64) int x;
    int pad[15];
};

static int va_sum_ints(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += va_arg(ap, int);
    }
    va_end(ap);
    return s;
}

static int (*volatile g_va_fn)(int, ...) = va_sum_ints;

/* VLA of over-aligned struct: every element must be 64-byte aligned. */
static int test_vla_over64(int n) {
    struct over64 a[n];
    int sum = 0;
    for (int i = 0; i < n; i++) {
        if (((uintptr_t)&a[i] & 63u) != 0u)
            return -100 - i;
        a[i].x = (i + 1) * 11;
        a[i].pad[14] = i * 3;
    }
    /* Trigger a call while VLA is live */
    int extra = g_va_fn(4, a[0].x, a[n - 1].x, a[0].pad[14], a[n - 1].pad[14]);
    for (int i = 0; i < n; i++) {
        sum += a[i].x + a[i].pad[14];
    }
    return sum + extra;
}

/* Combine: >16KB fixed local array (chkstk), _Alignas(64) scalar & array,
 * dynamic VLA, high GP+FP register pressure across a variadic call. */
static int test_big_frame_vla_alignas(int n, int seed) {
    unsigned char big_buf[20000];
    _Alignas(64) int al_scalar = seed + 7;
    _Alignas(64) int al_arr[8];
    int vla[n];

    if (((uintptr_t)&al_scalar & 63u) != 0u) return -1;
    if (((uintptr_t)&al_arr[0] & 63u) != 0u) return -2;

    for (int i = 0; i < 8; i++) al_arr[i] = seed + i * 2;
    for (int i = 0; i < n; i++) vla[i] = seed * 3 + i;
    big_buf[0] = (unsigned char)(seed & 0xFF);
    big_buf[9999] = (unsigned char)((seed + 13) & 0xFF);
    big_buf[19999] = (unsigned char)((seed + 29) & 0xFF);

    /* Force callee-saved spills across a call */
    int r0 = seed + 1, r1 = seed + 2, r2 = seed + 3, r3 = seed + 4;
    int r4 = seed + 5, r5 = seed + 6, r6 = seed + 7, r7 = seed + 8;
    int r8 = seed + 9, r9 = seed + 10, r10 = seed + 11, r11 = seed + 12;
    double d0 = (double)seed + 0.5, d1 = (double)seed + 1.5;
    double d2 = (double)seed + 2.5, d3 = (double)seed + 3.5;

    int cv = g_va_fn(6, r0, r3, r6, r9, al_scalar, vla[n - 1]);

    int total = cv + r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7 + r8 + r9 + r10 + r11;
    total += (int)(d0 + d1 + d2 + d3);
    total += al_scalar + al_arr[0] + al_arr[7] + vla[0] + vla[n - 1];
    total += (int)big_buf[0] + (int)big_buf[9999] + (int)big_buf[19999];
    return total;
}

/* Nested over-aligned VLA + inner loop VLA + goto out of inner VLA scope. */
static int test_nested_vla_goto(int outer_n, int iters) {
    struct over64 outer[outer_n];
    if (((uintptr_t)&outer[0] & 63u) != 0u) return -1;
    for (int i = 0; i < outer_n; i++) {
        outer[i].x = i * 5 + 1;
        outer[i].pad[0] = i + 10;
    }

    int acc = 0;
    for (int k = 0; k < iters; k++) {
        int inner_len = (k % 7) + 4;
        int inner[inner_len];
        for (int j = 0; j < inner_len; j++) inner[j] = k + j;
        if (k == iters - 1) {
            acc += inner[inner_len - 1];
            goto done;
        }
        acc += inner[0];
    }
done:
    /* Verify outer VLA is intact and still 64-byte aligned after stackrestore */
    if (((uintptr_t)&outer[0] & 63u) != 0u) return -2;
    for (int i = 0; i < outer_n; i++) {
        if (outer[i].x != i * 5 + 1 || outer[i].pad[0] != i + 10)
            return -3;
        acc += outer[i].x;
    }
    return acc;
}

static int (*volatile g_over64_fn)(int) = test_vla_over64;
static int (*volatile g_big_fn)(int, int) = test_big_frame_vla_alignas;
static int (*volatile g_nest_fn)(int, int) = test_nested_vla_goto;

int main(void) {
    C17_BEGIN("c67_bug_vla_overalign_and_frame",
              "Over-aligned VLAs, >16KB frame + alignas + VLA + spills + goto");

    /* 1. Over-aligned VLA (n=5):
     * sum(a[i].x) = 11*(1+2+3+4+5) = 165; sum(pad[14]) = 3*(0+1+2+3+4) = 30
     * extra = 11 + 55 + 0 + 12 = 78 -> total = 273 */
    C17_CHECK_EQ(g_over64_fn(c17_opaque_int(5)), 273);
    /* n=1: a[0].x=11, pad[14]=0, extra=22 -> 33 */
    C17_CHECK_EQ(g_over64_fn(c17_opaque_int(1)), 33);

    /* 2. Big frame (>16KB) + _Alignas(64) + VLA + register spills + variadic call:
     * seed=10, n=7:
     * al_scalar=17, al_arr[0]=10, al_arr[7]=24, vla[0]=30, vla[6]=36
     * r0..r11 = 11..22 (sum = 198), cv = 11+14+17+20+17+36 = 115
     * d0..d3 = 10.5+11.5+12.5+13.5 = 48
     * big_buf: 10 + 23 + 39 = 72
     * total = 115 + 198 + 48 + (17+10+24+30+36) + 72 = 550 */
    C17_CHECK_EQ(g_big_fn(c17_opaque_int(7), c17_opaque_int(10)), 550);

    /* 3. Nested VLA + goto stackrestore (outer_n=4, iters=64):
     * k=0..62: acc += k => 62*63/2 = 1953
     * k=63: inner_len = (63%7)+4 = 4, inner[3] = 63+3 = 66
     * outer[0..3].x = 1, 6, 11, 16 => sum = 34
     * total = 1953 + 66 + 34 = 2053 */
    C17_CHECK_EQ(g_nest_fn(c17_opaque_int(4), c17_opaque_int(64)), 2053);

    C17_END();
}
