/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.2.8 / §6.7.6.2 / §7.13 — Adversarial test for
 * x86-64 ____chkstk_darwin (>= 4KB and >= 16KB frames) combined with SysV
 * variadic register saves, _Alignas(32)/_Alignas(64) locals & VLAs across
 * varying stack depths, and setjmp/longjmp across callee-saved rbx/r12..r15.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "c17_test.h"

/* Variadic function with >= 20KB local frame: triggers ____chkstk_darwin on x86-64
 * BEFORE saving SysV rdi..r9 & xmm0..xmm7 into the register save area! */
static double big_frame_variadic(int g1, int g2, int g3, double f1, double f2, ...) {
    unsigned char buf[20480];
    for (int i = 0; i < 20480; i += 256) {
        buf[i] = (unsigned char)((g1 + i) & 0xFF);
    }

    va_list ap;
    va_start(ap, f2);
    int g4 = va_arg(ap, int);
    int g5 = va_arg(ap, int);
    int g6 = va_arg(ap, int);
    int g7 = va_arg(ap, int);
    double f3 = va_arg(ap, double);
    double f4 = va_arg(ap, double);
    double f5 = va_arg(ap, double);
    double f6 = va_arg(ap, double);
    double f7 = va_arg(ap, double);
    double f8 = va_arg(ap, double);
    double f9 = va_arg(ap, double);
    va_end(ap);

    int chk = (int)buf[0] + (int)buf[4096] + (int)buf[16384];
    return (double)(g1 + g2 + g3 + g4 + g5 + g6 + g7)
         + (f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9)
         + (double)chk;
}

/* Recurse through 8 depths with alternating extra frame sizes so rbp % 64 hits
 * all possible 16-byte residues; verify _Alignas(32), _Alignas(64), and overaligned VLAs. */
static int check_overalign_depths(int depth, int n) {
    _Alignas(32) int a32 = depth * 10 + 1;
    _Alignas(64) long long a64 = (long long)depth * 100 + 2;
    _Alignas(32) int vla32[n];
    _Alignas(64) long long vla64[n];
    /* Odd-sized padding array shifts stack depth mod 64 across recursive calls */
    volatile char pad[16];
    pad[0] = (char)depth;

    int ok = 1;
    if (((uintptr_t)&a32 & 31u) != 0u) ok = 0;
    if (((uintptr_t)&a64 & 63u) != 0u) ok = 0;
    if (((uintptr_t)vla32 & 31u) != 0u) ok = 0;
    if (((uintptr_t)vla64 & 63u) != 0u) ok = 0;

    for (int i = 0; i < n; ++i) {
        vla32[i] = depth + i;
        vla64[i] = (long long)(depth * 10 + i);
    }

    int sub = (depth > 0) ? check_overalign_depths(depth - 1, n + 1) : 1;

    /* Verify contents survived child call without stack corruption */
    if (a32 != depth * 10 + 1 || a64 != (long long)depth * 100 + 2) ok = 0;
    if (pad[0] != (char)depth) ok = 0;
    for (int i = 0; i < n; ++i) {
        if (vla32[i] != depth + i || vla64[i] != (long long)(depth * 10 + i)) ok = 0;
    }
    return ok && sub;
}

/* Deep helper that clobbers callee-saved regs (rbx, r12..r15) before calling longjmp */
static void clobber_and_jump(jmp_buf env, int val, int a, int b, int c, int d, int e) {
    volatile int sink = 0;
    int r1 = a * 3 + 1;
    int r2 = b * 5 + 2;
    int r3 = c * 7 + 3;
    int r4 = d * 11 + 4;
    int r5 = e * 13 + 5;
    sink = r1 + r2 + r3 + r4 + r5;
    if (sink != -9999999) {
        longjmp(env, val);
    }
}

/* setjmp/longjmp inside a >= 8KB chkstk frame with VLA and volatile state */
static int test_chkstk_vla_setjmp(int n) {
    unsigned char big[8192];
    big[0] = 0x5Au;
    big[4096] = 0xA5u;
    big[8191] = 0x3Cu;

    int vla[n];
    for (int i = 0; i < n; ++i) {
        vla[i] = (i + 1) * 7;
    }

    jmp_buf env;
    volatile int state = 10;
    volatile int trips = 0;
    /* Keep several non-modified locals alive across setjmp (ISO C17 §7.13.2.1p3) */
    int k1 = c17_opaque_int(11);
    int k2 = c17_opaque_int(22);
    int k3 = c17_opaque_int(33);
    int k4 = c17_opaque_int(44);
    int k5 = c17_opaque_int(55);

    int jv = setjmp(env);
    if (jv == 0) {
        state = 42;
        trips = 1;
        clobber_and_jump(env, 77, k1, k2, k3, k4, k5);
    } else if (jv == 77) {
        state += 8;
        trips += 1;
        clobber_and_jump(env, 99, k5, k4, k3, k2, k1);
    }

    int vsum = 0;
    for (int i = 0; i < n; ++i) {
        vsum += vla[i];
    }
    int ok_big = (big[0] == 0x5Au && big[4096] == 0xA5u && big[8191] == 0x3Cu);
    return ok_big ? (jv + state + trips * 100 + k1 + k2 + k3 + k4 + k5 + vsum) : -1;
}

int main(void) {
    C17_BEGIN("c67_bug_x64_chkstk_vla_setjmp_align",
              "x86-64 chkstk + variadic save, overaligned locals/VLAs, setjmp/longjmp");

    /* 1. Variadic with 20KB stack probe */
    {
        double r = big_frame_variadic(1, 2, 3, 1.5, 2.5,
                                      4, 5, 6, 7,
                                      3.5, 4.5, 5.5, 6.5, 7.5, 8.5, 9.5);
        /* g1..g7 = 28; f1..f9 = 49.5; chk = 1 + 1 + 1 = 3 -> 80.5 */
        C17_CHECK_DBL(r, 80.5, 1e-12);
    }

    /* 2. Overaligned locals & VLAs across 8 recursion depths */
    {
        C17_CHECK_EQ(check_overalign_depths(7, c17_opaque_int(3)), 1);
    }

    /* 3. chkstk + VLA + setjmp/longjmp across callee-saved register clobbers */
    {
        int n = c17_opaque_int(8);
        int r = test_chkstk_vla_setjmp(n);
        /* jv=99, state=50, trips=2 (200), k1..k5=165, vsum=7*(8*9/2)=252 -> 766 */
        C17_CHECK_EQ(r, 766);
    }

    C17_END();
}
