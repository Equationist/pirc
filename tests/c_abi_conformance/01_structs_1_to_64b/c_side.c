/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

typedef struct { int8_t a; } S1;
typedef struct { int16_t a; } S2;
typedef struct { uint8_t a, b, c; } S3;
typedef struct { int32_t a; } S4;
typedef struct { uint8_t a[5]; } S5;
typedef struct { uint8_t a[7]; } S7;
typedef struct { int32_t a, b; } S8;
typedef struct { int32_t a, b, c; } S12;
typedef struct { int64_t a, b; } S16;
typedef struct { int64_t a, b; int8_t c; } S17; /* sizeof == 24 on LP64 */
typedef struct { int64_t a, b, c; } S24;
typedef struct { int64_t a[4]; } S32;
typedef struct { int64_t a[8]; } S64;

/* Implemented in PIR */
extern S1  pir_rt_s1(S1 x);
extern S2  pir_rt_s2(S2 x);
extern S3  pir_rt_s3(S3 x);
extern S4  pir_rt_s4(S4 x);
extern S5  pir_rt_s5(S5 x);
extern S7  pir_rt_s7(S7 x);
extern S8  pir_rt_s8(S8 x);
extern S12 pir_rt_s12(S12 x);
extern S16 pir_rt_s16(S16 x);
extern S17 pir_rt_s17(S17 x);
extern S24 pir_rt_s24(S24 x);
extern S32 pir_rt_s32(S32 x);
extern S64 pir_rt_s64(S64 x);
extern int64_t pir_spill_s16(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                             int64_t a4, int64_t a5, int64_t a6, S16 s);
extern int64_t pir_drive_c_calls(void);

/* Implemented in C, called by PIR */
S3 c_rt_s3(S3 x) {
    S3 r = { (uint8_t)(x.a + 1), (uint8_t)(x.b + 2), (uint8_t)(x.c + 3) };
    x.a = 0xFF; /* Mutate byval copy to test caller isolation */
    return r;
}

S16 c_rt_s16(S16 x) {
    S16 r = { x.a + 10, x.b + 20 };
    x.a = -9999;
    return r;
}

S17 c_rt_s17(S17 x) {
    S17 r = { x.a + 1, x.b + 2, (int8_t)(x.c + 3) };
    x.a = -8888;
    return r;
}

S64 c_rt_s64(S64 x) {
    S64 r;
    for (int i = 0; i < 8; ++i) {
        r.a[i] = x.a[i] + (int64_t)(i + 1);
    }
    x.a[0] = -7777;
    return r;
}

int64_t c_spill_s16(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                    int64_t a4, int64_t a5, int64_t a6, S16 s) {
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + s.a * 3 + s.b * 5;
}

int main(void) {
    /* 1B..16B direct GP structs */
    S1 r1 = pir_rt_s1((S1){ 11 });
    CHECK_EQ_I64(r1.a, 12);

    S2 r2 = pir_rt_s2((S2){ 1000 });
    CHECK_EQ_I64(r2.a, 1005);

    S3 orig3 = { 10, 20, 30 };
    S3 r3 = pir_rt_s3(orig3);
    CHECK_EQ_I64(r3.a, 11); CHECK_EQ_I64(r3.b, 22); CHECK_EQ_I64(r3.c, 33);
    CHECK_EQ_I64(orig3.a, 10);

    S4 r4 = pir_rt_s4((S4){ -400 });
    CHECK_EQ_I64(r4.a, -390);

    S5 r5 = pir_rt_s5((S5){ { 1, 2, 3, 4, 5 } });
    CHECK_EQ_I64(r5.a[0], 2); CHECK_EQ_I64(r5.a[4], 10);

    S7 r7 = pir_rt_s7((S7){ { 3, 4, 5, 6, 7, 8, 9 } });
    CHECK_EQ_I64(r7.a[0], 4); CHECK_EQ_I64(r7.a[6], 16);

    S8 r8 = pir_rt_s8((S8){ 100, -50 });
    CHECK_EQ_I64(r8.a, 101); CHECK_EQ_I64(r8.b, -48);

    S12 r12 = pir_rt_s12((S12){ 7, 8, 9 });
    CHECK_EQ_I64(r12.a, 8); CHECK_EQ_I64(r12.b, 10); CHECK_EQ_I64(r12.c, 12);

    S16 orig16 = { 10000000000LL, -20000000000LL };
    S16 r16 = pir_rt_s16(orig16);
    CHECK_EQ_I64(r16.a, 10000000010LL);
    CHECK_EQ_I64(r16.b, -19999999980LL);
    CHECK_EQ_I64(orig16.a, 10000000000LL);

    /* >16B indirect / x8 retbuf structs */
    S17 orig17 = { 50, 60, 7 };
    S17 r17 = pir_rt_s17(orig17);
    CHECK_EQ_I64(r17.a, 51); CHECK_EQ_I64(r17.b, 62); CHECK_EQ_I64(r17.c, 10);
    CHECK_EQ_I64(orig17.a, 50);

    S24 r24 = pir_rt_s24((S24){ 11, 22, 33 });
    CHECK_EQ_I64(r24.a, 12); CHECK_EQ_I64(r24.b, 24); CHECK_EQ_I64(r24.c, 36);

    S32 r32 = pir_rt_s32((S32){ { 1, 2, 3, 4 } });
    CHECK_EQ_I64(r32.a[0], 2); CHECK_EQ_I64(r32.a[3], 8);

    S64 orig64 = { { 10, 20, 30, 40, 50, 60, 70, 80 } };
    S64 r64 = pir_rt_s64(orig64);
    CHECK_EQ_I64(r64.a[0], 11); CHECK_EQ_I64(r64.a[7], 88);
    CHECK_EQ_I64(orig64.a[0], 10);

    /* 7 GP args + 16B struct (must spill entire S16 to stack) */
    int64_t sp = pir_spill_s16(1, 2, 3, 4, 5, 6, 7, (S16){ 100, 200 });
    CHECK_EQ_I64(sp, 28 + 300 + 1000);

    /* PIR -> C direction */
    int64_t back = pir_drive_c_calls();
    CHECK_EQ_I64(back, 1);

    printf("PASS\n");
    return 0;
}
