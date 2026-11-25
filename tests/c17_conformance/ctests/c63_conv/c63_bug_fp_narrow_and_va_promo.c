/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.3.1.4 / §6.5.2.2 — Real-world small-compiler FP &
 * sub-word promotion bugs (TCC, QBE, chibicc, kefir):
 *   - double/float <-> int8_t, uint8_t, int16_t, uint16_t, int32_t, uint32_t
 *     boundary truncation toward zero
 *   - fneg sign-bit preservation on +0.0, -0.0, and NaN
 *   - Default argument promotions in variadic calls (float -> double,
 *     int8_t/uint8_t/int16_t/uint16_t/_Bool -> int)
 */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "c17_test.h"

static double neg_d(double x) { return -x; }
static float neg_f(float x) { return -x; }

static int va_default_promos(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int a = va_arg(ap, int);         /* promoted from int8_t (-5) */
    int b = va_arg(ap, int);         /* promoted from uint8_t (250) */
    int c = va_arg(ap, int);         /* promoted from int16_t (-1000) */
    int d = va_arg(ap, int);         /* promoted from uint16_t (60000) */
    int e = va_arg(ap, int);         /* promoted from _Bool (true -> 1) */
    double f = va_arg(ap, double);   /* promoted from float (3.5f -> 3.5) */
    double g = va_arg(ap, double);   /* promoted from float (-2.25f -> -2.25) */
    va_end(ap);
    int fsum = (int)((f + g) * 4.0); /* (3.5 - 2.25) * 4 = 5 */
    return a + b + c + d + e + fsum + count;
}

int main(void) {
    C17_BEGIN("c63_bug_fp_narrow_and_va_promo",
              "FP<->i8/u8/i16/u16/i32/u32, fneg -0.0/NaN, variadic default promotions");

    /* double/float -> narrow signed & unsigned integers (truncation toward zero) */
    {
        C17_CHECK_EQ((int8_t)c17_opaque_dbl(-128.9), -128);
        C17_CHECK_EQ((int8_t)c17_opaque_dbl(127.9), 127);
        C17_CHECK_EQ((int8_t)c17_opaque_dbl(-1.9), -1);
        C17_CHECK_EQ((uint8_t)c17_opaque_dbl(255.9), 255);
        C17_CHECK_EQ((uint8_t)c17_opaque_dbl(0.9), 0);

        C17_CHECK_EQ((int16_t)c17_opaque_dbl(-32768.9), -32768);
        C17_CHECK_EQ((int16_t)c17_opaque_dbl(32767.9), 32767);
        C17_CHECK_EQ((uint16_t)c17_opaque_dbl(65535.9), 65535);

        C17_CHECK_EQ((int32_t)c17_opaque_dbl(-2147483648.9), (int32_t)-2147483648LL);
        C17_CHECK_UEQ((uint32_t)c17_opaque_dbl(4294967295.9), 4294967295U);

        C17_CHECK_EQ((int8_t)c17_opaque_flt(-127.7f), -127);
        C17_CHECK_EQ((uint16_t)c17_opaque_flt(65000.9f), 65000);
        C17_CHECK_UEQ((uint32_t)c17_opaque_flt(2147483648.0f), 2147483648U);
    }

    /* Narrow signed & unsigned integers -> double/float */
    {
        int8_t s8 = (int8_t)c17_opaque_int(-128);
        uint8_t u8 = (uint8_t)c17_opaque_int(255);
        int16_t s16 = (int16_t)c17_opaque_int(-32768);
        uint16_t u16 = (uint16_t)c17_opaque_int(65535);
        uint32_t u32 = (uint32_t)c17_opaque_ull(0xFFFFFFFFULL);
        C17_CHECK_DBL((double)s8, -128.0, 0.0);
        C17_CHECK_DBL((double)u8, 255.0, 0.0);
        C17_CHECK_DBL((double)s16, -32768.0, 0.0);
        C17_CHECK_DBL((double)u16, 65535.0, 0.0);
        C17_CHECK_DBL((double)u32, 4294967295.0, 0.0);
        C17_CHECK_DBL((double)(float)s8, -128.0, 0.0);
        C17_CHECK_DBL((double)(float)u8, 255.0, 0.0);
        C17_CHECK_DBL((double)(float)s16, -32768.0, 0.0);
    }

    /* fneg sign-bit preservation on +0.0 and -0.0 (must NOT compile as 0.0 - x!) */
    {
        double pz = c17_opaque_dbl(0.0);
        double nz = c17_opaque_dbl(-0.0);
        C17_CHECK(signbit(neg_d(pz)) != 0);
        C17_CHECK(signbit(neg_d(nz)) == 0);

        float fpz = c17_opaque_flt(0.0f);
        float fnz = c17_opaque_flt(-0.0f);
        C17_CHECK(signbit(neg_f(fpz)) != 0);
        C17_CHECK(signbit(neg_f(fnz)) == 0);
    }

    /* Variadic default argument promotions */
    {
        int8_t a = (int8_t)c17_opaque_int(-5);
        uint8_t b = (uint8_t)c17_opaque_int(250);
        int16_t c = (int16_t)c17_opaque_int(-1000);
        uint16_t d = (uint16_t)c17_opaque_int(60000);
        _Bool e = (_Bool)c17_opaque_int(256);
        float f = c17_opaque_flt(3.5f);
        float g = c17_opaque_flt(-2.25f);
        /* -5 + 250 - 1000 + 60000 + 1 + 5 + 7 = 59258 */
        C17_CHECK_EQ(va_default_promos(c17_opaque_int(7), a, b, c, d, e, f, g), 59258);
    }

    C17_END();
}
