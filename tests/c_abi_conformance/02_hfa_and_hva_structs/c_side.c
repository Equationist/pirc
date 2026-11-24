/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

typedef struct { float a, b; } Hfa2F;
typedef struct { float a, b, c; } Hfa3F;
typedef struct { float a, b, c, d; } Hfa4F;
typedef struct { double a, b; } Hfa2D;
typedef struct { double a, b, c, d; } Hfa4D;
typedef struct { Hfa2F lo, hi; } NestedHfa4F;
typedef struct { float a[5]; } NonHfa5F;
typedef struct { v4f32_t a, b; } Hva2V4F;
typedef struct { v4f32_t a, b, c, d; } Hva4V4F;
typedef struct { v4f32_t a[5]; } NonHva5V4F;

/* Implemented in PIR */
extern Hfa2F       pir_rt_hfa2f(Hfa2F x);
extern Hfa3F       pir_rt_hfa3f(Hfa3F x);
extern Hfa4F       pir_rt_hfa4f(Hfa4F x);
extern Hfa2D       pir_rt_hfa2d(Hfa2D x);
extern Hfa4D       pir_rt_hfa4d(Hfa4D x);
extern NestedHfa4F pir_rt_nested_hfa4f(NestedHfa4F x);
extern NonHfa5F    pir_rt_non_hfa5f(NonHfa5F x);
extern Hva2V4F     pir_rt_hva2(Hva2V4F x);
extern Hva4V4F     pir_rt_hva4(Hva4V4F x);
extern NonHva5V4F  pir_rt_non_hva5(NonHva5V4F x);
extern float       pir_spill_hfa4f(float f0, float f1, float f2, float f3,
                                   float f4, float f5, Hfa4F h);
extern int32_t     pir_drive_c_hfa_hva(void);

/* Implemented in C, called by PIR */
Hfa4F c_rt_hfa4f(Hfa4F x) {
    return (Hfa4F){ x.a + 1.0f, x.b + 2.0f, x.c + 3.0f, x.d + 4.0f };
}

NonHfa5F c_rt_non_hfa5f(NonHfa5F x) {
    NonHfa5F r = x;
    r.a[0] += 1.0f;
    r.a[4] += 5.0f;
    return r;
}

Hva2V4F c_rt_hva2(Hva2V4F x) {
    v4f32_t one = { 1.0f, 1.0f, 1.0f, 1.0f };
    v4f32_t two = { 2.0f, 2.0f, 2.0f, 2.0f };
    return (Hva2V4F){ x.a + one, x.b + two };
}

float c_spill_hfa4f(float f0, float f1, float f2, float f3,
                    float f4, float f5, Hfa4F h) {
    return f0 + f1 + f2 + f3 + f4 + f5 + h.a + h.b + h.c + h.d;
}

int main(void) {
    Hfa2F r2f = pir_rt_hfa2f((Hfa2F){ 1.5f, 2.5f });
    CHECK_EQ_F64(r2f.a, 2.5); CHECK_EQ_F64(r2f.b, 4.5);

    Hfa3F r3f = pir_rt_hfa3f((Hfa3F){ 1.0f, 2.0f, 3.0f });
    CHECK_EQ_F64(r3f.a, 2.0); CHECK_EQ_F64(r3f.b, 4.0); CHECK_EQ_F64(r3f.c, 6.0);

    Hfa4F r4f = pir_rt_hfa4f((Hfa4F){ 10.0f, 20.0f, 30.0f, 40.0f });
    CHECK_EQ_F64(r4f.a, 11.0); CHECK_EQ_F64(r4f.b, 22.0);
    CHECK_EQ_F64(r4f.c, 33.0); CHECK_EQ_F64(r4f.d, 44.0);

    Hfa2D r2d = pir_rt_hfa2d((Hfa2D){ 100.25, -50.5 });
    CHECK_EQ_F64(r2d.a, 101.25); CHECK_EQ_F64(r2d.b, -48.5);

    Hfa4D r4d = pir_rt_hfa4d((Hfa4D){ 1.0, 2.0, 3.0, 4.0 });
    CHECK_EQ_F64(r4d.a, 2.0); CHECK_EQ_F64(r4d.d, 8.0);

    NestedHfa4F rnf = pir_rt_nested_hfa4f((NestedHfa4F){ { 1.0f, 2.0f }, { 3.0f, 4.0f } });
    CHECK_EQ_F64(rnf.lo.a, 2.0); CHECK_EQ_F64(rnf.hi.b, 8.0);

    NonHfa5F r5f = pir_rt_non_hfa5f((NonHfa5F){ { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f } });
    CHECK_EQ_F64(r5f.a[0], 2.0); CHECK_EQ_F64(r5f.a[4], 10.0);

    Hva2V4F rh2 = pir_rt_hva2((Hva2V4F){
        { 1.0f, 2.0f, 3.0f, 4.0f },
        { 5.0f, 6.0f, 7.0f, 8.0f }
    });
    CHECK_EQ_F64(rh2.a[0], 2.0); CHECK_EQ_F64(rh2.a[3], 5.0);
    CHECK_EQ_F64(rh2.b[0], 7.0); CHECK_EQ_F64(rh2.b[3], 10.0);

    Hva4V4F rh4 = pir_rt_hva4((Hva4V4F){
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 2.0f, 0.0f, 0.0f, 0.0f },
        { 3.0f, 0.0f, 0.0f, 0.0f },
        { 4.0f, 0.0f, 0.0f, 9.0f }
    });
    CHECK_EQ_F64(rh4.a[0], 2.0); CHECK_EQ_F64(rh4.d[3], 13.0);

    NonHva5V4F rh5 = pir_rt_non_hva5((NonHva5V4F){ {
        { 1.0f, 2.0f, 3.0f, 4.0f },
        { 0 }, { 0 }, { 0 },
        { 10.0f, 20.0f, 30.0f, 40.0f }
    } });
    CHECK_EQ_F64(rh5.a[0][0], 2.0); CHECK_EQ_F64(rh5.a[4][3], 45.0);

    /* 6 FP regs + Hfa4F (cannot split across s6..s7 and stack) */
    float sp = pir_spill_hfa4f(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f,
                               (Hfa4F){ 10.0f, 20.0f, 30.0f, 40.0f });
    CHECK_EQ_F64(sp, 121.0);

    CHECK_EQ_I64(pir_drive_c_hfa_hva(), 1);
    printf("PASS\n");
    return 0;
}
