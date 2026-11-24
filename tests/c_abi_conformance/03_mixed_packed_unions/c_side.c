/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

typedef struct { int32_t i; float f; } Mixed8;
typedef struct { int64_t i; double d; } Mixed16;
typedef struct { int8_t a; int64_t b; int8_t c; float d; } MixedPad24;
typedef struct __attribute__((packed)) { int8_t tag; int64_t val; } Packed9;
typedef union { int64_t i; double d; struct { int32_t lo, hi; } p; } Union8;
typedef union { struct { int64_t a, b; } i2; struct { double x, y; } d2; } Union16;
typedef union { int64_t a[3]; double d[3]; } Union24;

/* Implemented in PIR */
extern Mixed8     pir_rt_mixed8(Mixed8 x);
extern Mixed16    pir_rt_mixed16(Mixed16 x);
extern MixedPad24 pir_rt_mixed_pad(MixedPad24 x);
extern Packed9    pir_rt_packed9(Packed9 x);
extern Union8     pir_rt_union8_d2i(Union8 x);
extern Union16    pir_rt_union16(Union16 x);
extern Union24    pir_rt_union24(Union24 x);
extern int32_t    pir_drive_c_mixed(void);

/* Implemented in C, called by PIR */
Mixed16 c_rt_mixed16(Mixed16 x) {
    return (Mixed16){ x.i + 100, x.d + 2.5 };
}

Packed9 c_rt_packed9(Packed9 x) {
    Packed9 r;
    r.tag = (int8_t)(x.tag + 1);
    r.val = x.val + 1000;
    return r;
}

Union8 c_rt_union8_i2d(Union8 x) {
    Union8 r;
    r.d = (double)x.i + 0.5;
    return r;
}

int main(void) {
    CHECK_EQ_U64(sizeof(MixedPad24), 24);
    CHECK_EQ_U64(sizeof(Packed9), 9);

    Mixed8 m8 = pir_rt_mixed8((Mixed8){ 42, 3.5f });
    CHECK_EQ_I64(m8.i, 43); CHECK_EQ_F64(m8.f, 5.5);

    /* Mixed16 is NOT an HFA: passed in x0, x1 on AAPCS64 */
    Mixed16 m16 = pir_rt_mixed16((Mixed16){ -77, 12.25 });
    CHECK_EQ_I64(m16.i, -67); CHECK_EQ_F64(m16.d, 15.75);

    MixedPad24 mp = pir_rt_mixed_pad((MixedPad24){ 5, 1000000, 9, 2.5f });
    CHECK_EQ_I64(mp.a, 6); CHECK_EQ_I64(mp.b, 1000002);
    CHECK_EQ_I64(mp.c, 12); CHECK_EQ_F64(mp.d, 6.5);

    Packed9 p9 = pir_rt_packed9((Packed9){ 7, 0x1122334455667700LL });
    CHECK_EQ_I64(p9.tag, 8);
    CHECK_EQ_I64(p9.val, 0x1122334455667701LL);

    /* Union8: pass double 2.0 (0x4000000000000000), PIR reads upper 32 bits */
    Union8 u8_in;
    u8_in.d = 2.0;
    Union8 u8_out = pir_rt_union8_d2i(u8_in);
    CHECK_EQ_U64((uint64_t)u8_out.i, 0x40000000ULL);

    Union16 u16_in;
    u16_in.i2.a = 10; u16_in.i2.b = 20;
    Union16 u16_out = pir_rt_union16(u16_in);
    CHECK_EQ_I64(u16_out.i2.a, 15); CHECK_EQ_I64(u16_out.i2.b, 27);

    Union24 u24_in;
    u24_in.a[0] = 100; u24_in.a[1] = 200; u24_in.a[2] = 300;
    Union24 u24_out = pir_rt_union24(u24_in);
    CHECK_EQ_I64(u24_out.a[0], 101); CHECK_EQ_I64(u24_out.a[2], 303);

    CHECK_EQ_I64(pir_drive_c_mixed(), 1);
    printf("PASS\n");
    return 0;
}
