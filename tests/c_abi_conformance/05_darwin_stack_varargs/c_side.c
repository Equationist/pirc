/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"
#include <stdarg.h>

typedef struct { int32_t x, y; } Pair;
typedef struct { int64_t a, b, c; } Big24;

/* Implemented in PIR */
extern int64_t pir_sum_varargs(int32_t tag, ...);
extern int32_t pir_format_via_vsnprintf(char *buf, int64_t cap, const char *fmt, ...);
extern int32_t pir_drive_c_varargs(char *out_buf, int64_t cap);

/* Implemented in C, called by PIR */
int64_t c_consume_varargs(int32_t tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int32_t a = va_arg(ap, int32_t);
    int64_t b = va_arg(ap, int64_t);
    double  c = va_arg(ap, double);
    Pair    p = va_arg(ap, Pair);
    Big24   g = va_arg(ap, Big24);
    va_end(ap);
    return (int64_t)tag + (int64_t)a + b + (int64_t)c + (int64_t)p.x + (int64_t)p.y
           + g.a + g.b + g.c;
}

int main(void) {
    /* 1. C -> PIR variadic function reading i32, i64, f64, Pair, plus va_copy */
    Pair p = { 10, 20 };
    int64_t s = pir_sum_varargs(5, (int32_t)7, (int64_t)100, 12.0, p);
    /* Expected: tag(5) + a(7) + b(100) + c(12) + p.x(10) + p.y(20) + copy_a(7) = 161 */
    CHECK_EQ_I64(s, 161);

    /* 2. C -> PIR -> C vsnprintf (tests Darwin va_list == char* passing) */
    char buf1[128];
    int32_t n1 = pir_format_via_vsnprintf(buf1, (int64_t)sizeof(buf1),
                                          "k=%d s=%s f=%.2f", 42, "mac", 3.25);
    CHECK(n1 > 0);
    CHECK(strcmp(buf1, "k=42 s=mac f=3.25") == 0);

    /* 3. PIR -> C snprintf + custom C varargs with byval Pair & Big24 */
    char buf2[128];
    int32_t ok = pir_drive_c_varargs(buf2, (int64_t)sizeof(buf2));
    CHECK_EQ_I64(ok, 1);
    CHECK(strcmp(buf2, "pir:42:3.25:ok") == 0);

    printf("PASS\n");
    return 0;
}
