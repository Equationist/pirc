/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.16 — Bidirectional slimcc-pir <-> clang variadic
 * struct ABI interoperability (small, 3-float HFA, 12B, 17B indirect, 4-double HFA).
 */
#include <stdarg.h>
#include "c17_test.h"
#include "interop.h"

int slimcc_va_sum(int n, ...) {
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

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_varargs",
              "Bidirectional slimcc-pir <-> clang variadic struct ABI interop");

    /* Direction 1: slimcc-pir caller -> clang variadic callee */
    {
        struct va_s3 a = { { 1, 2, 3 } };
        struct va_f3 hf = { 2.0f, 3.0f, 4.0f };
        struct va_s12 b = { 10, 20, 30 };
        struct va_s17 c = { { 0 } };
        c.b[0] = 7;
        c.b[8] = 5;
        c.b[16] = 13;
        struct va_d4 hd = { 1.0, 2.0, 3.0, 4.0 };
        /* (1+2+3) + 5 + (2+3+4) + (10+20+30) + (7+5+13) + (1+2+3+4) + 100 + 4 = 219 */
        int r1 = clang_va_sum(c17_opaque_int(4), a, c17_opaque_dbl(5.0),
                              hf, b, c, hd, c17_opaque_int(100));
        C17_CHECK_EQ(r1, 219);
    }

    /* Direction 2: clang caller -> slimcc-pir variadic callee */
    {
        int r2 = clang_calls_slimcc_va(c17_opaque_int(4));
        C17_CHECK_EQ(r2, 219);
    }

    C17_END();
}
