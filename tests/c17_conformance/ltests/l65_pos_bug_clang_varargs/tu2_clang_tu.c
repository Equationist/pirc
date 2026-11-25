/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: verifies true binary ABI compatibility with slimcc-pir
 * for variadic small structs, HFAs (3-float, 4-double), and >16B indirect structs.
 */
#include <stdarg.h>
#include "interop.h"

int clang_va_sum(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int total = 0;
    struct va_s3 a = va_arg(ap, struct va_s3);
    total += a.b[0] + a.b[1] + a.b[2];
    double d = va_arg(ap, double);
    total += (int)d;
    struct va_f3 hf = va_arg(ap, struct va_f3);
    total += (int)(hf.a + hf.b + hf.c);
    struct va_s12 b = va_arg(ap, struct va_s12);
    total += (int)(b.a + b.b + b.c);
    struct va_s17 c = va_arg(ap, struct va_s17);
    total += c.b[0] + c.b[8] + c.b[16];
    struct va_d4 hd = va_arg(ap, struct va_d4);
    total += (int)(hd.a + hd.b + hd.c + hd.d);
    int tail = va_arg(ap, int);
    total += tail + n;
    va_end(ap);
    return total;
}

int clang_calls_slimcc_va(int n) {
    struct va_s3 a = { { 1, 2, 3 } };
    struct va_f3 hf = { 2.0f, 3.0f, 4.0f };
    struct va_s12 b = { 10, 20, 30 };
    struct va_s17 c = { { 0 } };
    c.b[0] = 7;
    c.b[8] = 5;
    c.b[16] = 13;
    struct va_d4 hd = { 1.0, 2.0, 3.0, 4.0 };
    return slimcc_va_sum(n, a, 5.0, hf, b, c, hd, 100);
}
