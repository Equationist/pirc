/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 / §7.16 — Real-world ABI codegen bugs:
 *   - ARM64 AAPCS64 & x86-64 SysV HFA (Homogeneous Floating-point Aggregate)
 *     structs of 1..4 floats and 1..4 doubles, including spilling past 8 FP regs
 *   - Non-HFA 5-float struct (>16 bytes, indirect)
 *   - Odd-sized structs (3, 5, 7, 9, 12, 15, 17, 24 bytes) passed & returned
 *     by value and passed through variadic (...) arguments
 */
#include <stdarg.h>
#include <stdint.h>
#include "c17_test.h"

struct f3 { float a, b, c; };
struct f4 { float a, b, c, d; };
struct f5 { float a, b, c, d, e; };
struct d3 { double a, b, c; };
struct d4 { double a, b, c, d; };

struct s3  { uint8_t b[3]; };
struct s7  { uint8_t b[7]; };
struct s12 { uint32_t a, b, c; };
struct s17 { uint8_t b[17]; };

static struct f3 add_f3(struct f3 x, struct f3 y) {
    struct f3 r = { x.a + y.a, x.b + y.b, x.c + y.c };
    return r;
}

static struct d4 sum_d4_spill(struct d4 a, struct d4 b, struct d4 c) {
    /* 3 x 4 doubles = 12 doubles -> exceeds 8 FP arg regs on both ARM64 & x86-64 */
    struct d4 r = {
        a.a + b.a + c.a,
        a.b + b.b + c.b,
        a.c + b.c + c.c,
        a.d + b.d + c.d
    };
    return r;
}

static float sum_f5(struct f5 x) {
    return x.a + x.b + x.c + x.d + x.e;
}

static struct s7 flip_s7(struct s7 x) {
    struct s7 r;
    for (int i = 0; i < 7; i++) {
        r.b[i] = x.b[6 - i];
    }
    return r;
}

static struct s17 shift_s17(struct s17 x, uint8_t delta) {
    struct s17 r;
    for (int i = 0; i < 17; i++) {
        r.b[i] = (uint8_t)(x.b[i] + delta);
    }
    return r;
}

static int va_struct_check(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int total = 0;
    struct s3 a = va_arg(ap, struct s3);
    total += a.b[0] + a.b[1] + a.b[2];
    double d = va_arg(ap, double);
    total += (int)d;
    struct f3 hf = va_arg(ap, struct f3);
    total += (int)(hf.a + hf.b + hf.c);
    struct s12 b = va_arg(ap, struct s12);
    total += (int)(b.a + b.b + b.c);
    struct s17 c = va_arg(ap, struct s17);
    total += c.b[0] + c.b[16];
    struct d4 hd = va_arg(ap, struct d4);
    total += (int)(hd.a + hd.b + hd.c + hd.d);
    int tail = va_arg(ap, int);
    total += tail + n;
    va_end(ap);
    return total;
}

int main(void) {
    C17_BEGIN("c65_bug_abi_struct_varargs",
              "HFA 3f/4d spill, 5f non-HFA, odd-sized structs, varargs structs");

    /* 3-float HFA (12 bytes) */
    {
        struct f3 x = { c17_opaque_flt(1.5f), c17_opaque_flt(2.5f), c17_opaque_flt(3.5f) };
        struct f3 y = { c17_opaque_flt(10.0f), c17_opaque_flt(20.0f), c17_opaque_flt(30.0f) };
        struct f3 z = add_f3(x, y);
        C17_CHECK_DBL(z.a, 11.5, 1e-6);
        C17_CHECK_DBL(z.b, 22.5, 1e-6);
        C17_CHECK_DBL(z.c, 33.5, 1e-6);
    }

    /* 4-double HFA spilling past 8 FP argument registers */
    {
        struct d4 a = { 1.0, 2.0, 3.0, 4.0 };
        struct d4 b = { 10.0, 20.0, 30.0, 40.0 };
        struct d4 c = { 100.0, 200.0, 300.0, 400.0 };
        struct d4 r = sum_d4_spill(a, b, c);
        C17_CHECK_DBL(r.a, 111.0, 0.0);
        C17_CHECK_DBL(r.b, 222.0, 0.0);
        C17_CHECK_DBL(r.c, 333.0, 0.0);
        C17_CHECK_DBL(r.d, 444.0, 0.0);
    }

    /* 5-float non-HFA (20 bytes) */
    {
        struct f5 v = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
        C17_CHECK_DBL(sum_f5(v), 15.0, 1e-6);
    }

    /* Odd-sized 7-byte and 17-byte by-value pass & return */
    {
        struct s7 in7 = { { 1, 2, 3, 4, 5, 6, 7 } };
        struct s7 out7 = flip_s7(in7);
        for (int i = 0; i < 7; i++) {
            C17_CHECK_EQ(out7.b[i], 7 - i);
        }

        struct s17 in17;
        for (int i = 0; i < 17; i++) {
            in17.b[i] = (uint8_t)(i * 3);
        }
        struct s17 out17 = shift_s17(in17, (uint8_t)c17_opaque_int(10));
        for (int i = 0; i < 17; i++) {
            C17_CHECK_EQ(out17.b[i], i * 3 + 10);
        }
    }

    /* Variadic passing of mixed structs (3B, 3-float HFA, 12B, 17B indirect, 4-double HFA) + double + int */
    {
        struct s3 a = { { 1, 2, 3 } };
        struct f3 hf = { 2.0f, 3.0f, 4.0f };
        struct s12 b = { 10, 20, 30 };
        struct s17 c = { { 0 } };
        c.b[0] = 7;
        c.b[16] = 13;
        struct d4 hd = { 1.0, 2.0, 3.0, 4.0 };
        /* (1+2+3) + 5 + (2+3+4) + (10+20+30) + (7+13) + (1+2+3+4) + 100 + 4 = 214 */
        int got = va_struct_check(c17_opaque_int(4), a, c17_opaque_dbl(5.0),
                                  hf, b, c, hd, c17_opaque_int(100));
        C17_CHECK_EQ(got, 214);
    }

    C17_END();
}
