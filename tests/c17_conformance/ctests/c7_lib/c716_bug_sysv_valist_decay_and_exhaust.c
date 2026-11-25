/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.16 — Adversarial test for SysV AMD64 va_list
 * array-decay parameter passing, independent va_copy cursors, all-or-nothing
 * GP/XMM register exhaustion with subsequent single-reg reads, and _Alignas(16)
 * structs inside overflow_arg_area.
 */
#include <stdarg.h>
#include <stdint.h>
#include "c17_test.h"

struct pair_ll {
    long long a;
    long long b;
};

struct pair_dd {
    double a;
    double b;
};

struct mix_id {
    int a;
    double b;
};

struct al16_s {
    _Alignas(16) long long x;
    long long y;
    long long z;
};

/* Helper taking va_list by parameter (decayed pointer on x86-64, value on ARM64) */
static long long helper_sum_ints(int count, va_list ap) {
    long long s = 0;
    for (int i = 0; i < count; ++i) {
        s += (long long)va_arg(ap, int) * (long long)(i + 1);
    }
    return s;
}

static double helper_sum_mixed(va_list ap) {
    int a = va_arg(ap, int);
    double b = va_arg(ap, double);
    struct mix_id m = va_arg(ap, struct mix_id);
    return (double)a + b * 10.0 + (double)m.a * 100.0 + m.b * 1000.0;
}

static long long test_valist_forward(int count, ...) {
    va_list ap, cp1, cp2;
    va_start(ap, count);
    va_copy(cp1, ap);
    va_copy(cp2, ap);

    long long r1 = helper_sum_ints(count, cp1);
    /* Independent cursor cp2 still sees the exact same arguments from the beginning */
    long long r2 = 0;
    for (int i = 0; i < count; ++i) {
        r2 += (long long)va_arg(cp2, int) * (long long)(i + 1);
    }
    va_end(cp2);
    va_end(cp1);
    va_end(ap);
    return (r1 == r2) ? r1 : -1;
}

static double test_valist_mixed_forward(int dummy, ...) {
    va_list ap;
    va_start(ap, dummy);
    double r = helper_sum_mixed(ap);
    va_end(ap);
    return r + (double)dummy;
}

/* 5 GP used (fixed tag + 4 int va_args), then 16B [INT,INT] spills -> next int STILL in r9! */
static long long va_gp_exhaust(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int g2 = va_arg(ap, int);
    int g3 = va_arg(ap, int);
    int g4 = va_arg(ap, int);
    int g5 = va_arg(ap, int);
    struct pair_ll p = va_arg(ap, struct pair_ll); /* 5+2 > 6 -> overflow_arg_area! */
    int g6_r9 = va_arg(ap, int);                   /* still in r9! */
    int g7_stk = va_arg(ap, int);                  /* overflow_arg_area after p! */
    struct al16_s a16 = va_arg(ap, struct al16_s); /* 16-byte aligned in overflow_arg_area! */
    int tail = va_arg(ap, int);
    va_end(ap);
    return (long long)tag + g2 * 2 + g3 * 3 + g4 * 4 + g5 * 5
         + p.a * 10 + p.b * 20
         + (long long)g6_r9 * 100 + (long long)g7_stk * 1000
         + a16.x + a16.y * 2 + a16.z * 3 + (long long)tail * 10000;
}

/* 7 XMM used, then 16B [SSE,SSE] spills -> next double STILL in xmm7! */
static double va_fp_exhaust(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    double sum = (double)tag;
    for (int i = 0; i < 7; ++i) {
        sum += va_arg(ap, double);
    }
    struct pair_dd dp = va_arg(ap, struct pair_dd); /* 7+2 > 8 -> overflow_arg_area! */
    double f8_xmm7 = va_arg(ap, double);            /* still in xmm7! */
    double f9_stk  = va_arg(ap, double);            /* overflow_arg_area after dp! */
    va_end(ap);
    return sum + dp.a * 10.0 + dp.b * 20.0 + f8_xmm7 * 100.0 + f9_stk * 1000.0;
}

/* 5 GP + 7 XMM used -> mix_id fits in (r9, xmm7); 6 GP + 7 XMM -> mix_id spills, next double in xmm7! */
static double va_mixed_boundary(int mode, ...) {
    va_list ap;
    va_start(ap, mode);
    double sum = 0.0;
    int ngp = (mode == 0) ? 4 : 5; /* +1 for fixed `mode` => 5 or 6 GP regs consumed */
    for (int i = 0; i < ngp; ++i) {
        sum += (double)va_arg(ap, int);
    }
    for (int i = 0; i < 7; ++i) {
        sum += va_arg(ap, double);
    }
    struct mix_id m = va_arg(ap, struct mix_id);
    double next_d = va_arg(ap, double);
    int next_i = va_arg(ap, int);
    va_end(ap);
    return sum + (double)m.a * 10.0 + m.b * 20.0 + next_d * 100.0 + (double)next_i * 1000.0;
}

int main(void) {
    C17_BEGIN("c716_bug_sysv_valist_decay_and_exhaust",
              "SysV AMD64 va_list decay, va_copy, GP/XMM exhaustion, 16B overflow align");

    /* 1. va_list passed to helper + va_copy independent cursor (10 args -> spans regs + stack) */
    {
        long long s = test_valist_forward(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
        /* sum_{i=1..10} i^2 = 385 */
        C17_CHECK_EQ(s, 385LL);

        struct mix_id m = { 7, 3.5 };
        double mf = test_valist_mixed_forward(5, 11, 2.25, m);
        /* 5 + 11 + 22.5 + 700 + 3500 = 4238.5 */
        C17_CHECK_DBL(mf, 4238.5, 1e-12);
    }

    /* 2. GP exhaustion + r9 reuse + _Alignas(16) in overflow_arg_area after odd stack slot */
    {
        struct pair_ll p = { 13, 17 };
        struct al16_s a16 = { 101, 202, 303 };
        long long r = va_gp_exhaust(1, 2, 3, 4, 5, p, 6, 7, a16, 8);
        /* 1+4+9+16+25 + 130+340 + 600 + 7000 + (101+404+909) + 80000 = 89539 */
        C17_CHECK_EQ(r, 89539LL);
    }

    /* 3. XMM exhaustion + xmm7 reuse */
    {
        struct pair_dd dp = { 2.5, 3.5 };
        double r = va_fp_exhaust(1, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, dp, 8.0, 9.0);
        /* 1 + 28 + 25 + 70 + 800 + 9000 = 9924.0 */
        C17_CHECK_DBL(r, 9924.0, 1e-12);
    }

    /* 4. Mixed GP+XMM boundary: fit vs all-or-nothing spill */
    {
        struct mix_id m = { 5, 1.5 };
        /* mode 0: 5 GP + 7 XMM -> m fits in r9 + xmm7 */
        double r0 = va_mixed_boundary(0, 1, 2, 3, 4,
                                      1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                      m, 9.0, 11);
        /* 10 + 28 + 50 + 30 + 900 + 11000 = 12018.0 */
        C17_CHECK_DBL(r0, 12018.0, 1e-12);

        /* mode 1: 6 GP + 7 XMM -> m spills to overflow, next_d STILL in xmm7! */
        double r1 = va_mixed_boundary(1, 1, 2, 3, 4, 5,
                                      1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                      m, 9.0, 11);
        /* 15 + 28 + 50 + 30 + 900 + 11000 = 12023.0 */
        C17_CHECK_DBL(r1, 12023.0, 1e-12);
    }

    C17_END();
}
