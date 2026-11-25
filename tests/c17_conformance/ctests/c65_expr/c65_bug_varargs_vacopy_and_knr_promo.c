/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.16, §6.5.2.2 — Adversarial va_copy, passing
 * va_list * to helper functions, partially-consumed va_copy, >12 mixed
 * variadic arguments, and K&R unprototyped default argument promotions.
 */
#include <stdarg.h>
#include <stdint.h>
#include "c17_test.h"

struct pair_d {
    double x;
    double y;
};

static int pull_two_from_ptr(va_list *app) {
    int a = va_arg(*app, int);
    double b = va_arg(*app, double);
    return a + (int)b;
}

static int (*volatile g_pull_ptr)(va_list *) = pull_two_from_ptr;

static int test_vacopy_nested(int tag, ...) {
    va_list ap1, ap2, ap3;
    va_start(ap1, tag);

    /* Copy before consuming anything */
    va_copy(ap2, ap1);

    /* Consume first 2 args via va_list* helper on ap1 */
    int r1 = g_pull_ptr(&ap1);

    /* Copy mid-stream from ap1 into ap3 */
    va_copy(ap3, ap1);

    /* Consume remaining from ap1 */
    struct pair_d p1 = va_arg(ap1, struct pair_d);
    int tail1 = va_arg(ap1, int);

    /* Consume remaining from ap3 (must match p1 & tail1 independently!) */
    struct pair_d p3 = va_arg(ap3, struct pair_d);
    int tail3 = va_arg(ap3, int);

    /* Consume everything from ap2 from the beginning */
    int a2 = va_arg(ap2, int);
    double b2 = va_arg(ap2, double);
    struct pair_d p2 = va_arg(ap2, struct pair_d);
    int tail2 = va_arg(ap2, int);

    va_end(ap3);
    va_end(ap2);
    va_end(ap1);

    if (r1 != a2 + (int)b2) return -1;
    if ((int)(p1.x + p1.y) != (int)(p2.x + p2.y)) return -2;
    if ((int)(p1.x + p1.y) != (int)(p3.x + p3.y)) return -3;
    if (tail1 != tail2 || tail1 != tail3) return -4;

    return tag + r1 + (int)(p1.x + p1.y) + tail1;
}

/* 14 variadic args crossing register/stack boundaries (mixed int/double/char/short/float) */
static int sum_many_va(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int acc = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == 'i') {
            acc += va_arg(ap, int);
        } else if (*p == 'd') {
            acc += (int)va_arg(ap, double);
        } else if (*p == 'l') {
            acc += (int)va_arg(ap, long long);
        }
    }
    va_end(ap);
    return acc;
}

/* K&R old-style function definition: exercises C17 default argument promotions */
static int knr_promo(a, b, c)
    int a;
    double b;
    int c;
{
    return a + (int)(b * 4.0) + c;
}

int main(void) {
    C17_BEGIN("c65_bug_varargs_vacopy_and_knr_promo",
              "va_copy mid-stream, va_list*, 14 mixed va_args, K&R promotions");

    /* 1. va_copy + va_list* helper + mid-stream copy */
    {
        struct pair_d pd = { 11.0, 19.0 };
        signed char sc = -5;
        float fl = 12.0f; /* default-promoted to double */
        /* tag=100, r1=(-5 + 12)=7, pd=(11+19)=30, tail=40 => 177 */
        int r = test_vacopy_nested(c17_opaque_int(100), sc, fl, pd, c17_opaque_int(40));
        C17_CHECK_EQ(r, 177);
    }

    /* 2. 14 mixed variadic args with default promotions */
    {
        signed char c1 = -3;
        unsigned char c2 = 200u;
        short s1 = -10;
        unsigned short s2 = 300u;
        float f1 = 2.0f, f2 = 4.0f, f3 = 6.0f;
        /* -3 + 2 + 200 + 4 + (-10) + 6 + 300 + 8 + 10 + 12 + 14 + 16 + 18 + 20 = 597 */
        int total = sum_many_va("idididididilii",
                                c1, f1, c2, f2, s1, f3, s2, 8.0,
                                10, 12.0, 14, 16LL, 18, 20);
        C17_CHECK_EQ(total, 597);
    }

    /* 3. K&R old-style call with float -> double and signed char/short -> int */
    {
        signed char a = (signed char)c17_opaque_int(-4);
        float b = (float)c17_opaque_dbl(2.5); /* promoted to double 2.5 -> * 4.0 = 10 */
        short c = (short)c17_opaque_int(100);
        C17_CHECK_EQ(knr_promo(a, b, c), 106);
    }

    C17_END();
}
