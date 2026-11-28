/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Bidirectional AAPCS32-VFP C <-> PIR ABI conformance harness compiled by
 * clang --target=thumbv7em-none-eabihf -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard.
 */

#include <stdarg.h>
#include <stdint.h>

typedef struct { float a, b, c, d; } Hfa4f;
typedef struct { double x, y, z; }   Hfa3d;
typedef struct { uint8_t a; int16_t b; } Small4;
typedef struct { int32_t x, y, z; }  Mid12;

/* Implemented in PIR (c_abi_check.pir) */
extern double pir_vfp_backfill(float a, double b, float c, double d, float e);
extern Hfa4f  pir_make_hfa4f(float a, float b, float c, float d);
extern float  pir_sum_hfa4f(float tag, double x, Hfa4f h);
extern Hfa3d  pir_make_hfa3d(double x, double y, double z);
extern Small4 pir_make_small4(uint8_t a, int16_t b);
extern int32_t pir_eval_mid12(int32_t k0, int32_t k1, Mid12 m);
extern int64_t pir_i64_align(int32_t tag, int64_t a, int64_t b);

/* Called from PIR */
double c_vfp_backfill(float a, double b, float c, double d, float e) {
    return (double)a + b + (double)c + d + (double)e;
}

Hfa4f c_make_hfa4f(float a, float b, float c, float d) {
    Hfa4f r = { a, b, c, d };
    return r;
}

int32_t c_sum_va(int32_t tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int32_t a = va_arg(ap, int32_t);
    double  d = va_arg(ap, double);
    int64_t b = va_arg(ap, int64_t);
    int32_t c = va_arg(ap, int32_t);
    va_end(ap);
    return tag + a + (int32_t)d + (int32_t)b + c;
}

extern int32_t pir_run_checks(void);

int main(void) {
    /* 1. C -> PIR: VFP backfilling (f32, f64, f32, f64, f32) */
    if (pir_vfp_backfill(1.5f, 2.25, 3.5f, 4.75, 5.0f) != 17.0) return 1;

    /* 2. C -> PIR: HFA4f return & byval with preceding (f32, f64) */
    Hfa4f h4 = pir_make_hfa4f(1.0f, 2.0f, 3.0f, 4.0f);
    if (h4.a != 1.0f || h4.b != 2.0f || h4.c != 3.0f || h4.d != 4.0f) return 2;
    if (pir_sum_hfa4f(5.0f, 6.0, h4) != 21.0f) return 3;

    /* 3. C -> PIR: HFA3d return (d0..d2) */
    Hfa3d h3 = pir_make_hfa3d(10.5, 20.5, 30.0);
    if (h3.x != 10.5 || h3.y != 20.5 || h3.z != 30.0) return 4;

    /* 4. C -> PIR: Small4 (<=4B in r0) and Mid12 (split across r2, r3 + stack) */
    Small4 sm = pir_make_small4(9, -300);
    if (sm.a != 9 || sm.b != -300) return 5;
    Mid12 m = { 10, 20, 30 };
    if (pir_eval_mid12(1, 2, m) != 63) return 6;

    /* 5. C -> PIR: i64 even-register alignment (r0, r2:r3, 8B stack) */
    if (pir_i64_align(12, 10000000000LL, 23456789000LL) != 33456789012LL) return 7;

    /* 6. PIR -> C checks */
    return pir_run_checks();
}
