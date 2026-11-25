/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared types and declarations for bidirectional slimcc-pir <-> clang
 * _Alignas(16) / _Alignas(32) mixed-struct & HFA stack/variadic ABI interop.
 */
#ifndef INTEROP_ALIGNAS16_H
#define INTEROP_ALIGNAS16_H

#include <stdint.h>

typedef struct {
    _Alignas(16) int a;
    double b;
} A16Mix;

typedef struct {
    _Alignas(16) float v[4];
} A16Hfa;

typedef struct {
    _Alignas(32) int a;
    double b;
} A32Big;

typedef struct { uint8_t a[3]; } Odd3;
typedef struct { uint8_t a[9]; } Odd9;
typedef struct { float x, y, z; } Float3;

/* Implemented in tu2_clang_tu.c (compiled by Clang) */
double clang_take_a16_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                            int i7, int i8, int stk1, A16Mix s);
float  clang_take_hfa16_stack(float f0, float f1, float f2, float f3,
                              float f4, float f5, float f6, float f7,
                              int i0, int i1, int i2, int i3,
                              int i4, int i5, int i6, int i7,
                              int stk1, A16Hfa h);
double clang_take_a32_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                            int i7, int i8, int stk1, A32Big b);
double clang_va_alignas16(int n, ...);

/* Implemented in tu1_main.c (compiled by slimcc-pir) */
double slimcc_take_a16_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                             int i7, int i8, int stk1, A16Mix s);
float  slimcc_take_hfa16_stack(float f0, float f1, float f2, float f3,
                               float f4, float f5, float f6, float f7,
                               int i0, int i1, int i2, int i3,
                               int i4, int i5, int i6, int i7,
                               int stk1, A16Hfa h);
double slimcc_take_a32_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                             int i7, int i8, int stk1, A32Big b);
double slimcc_va_alignas16(int n, ...);

/* Clang-compiled reverse-direction driver calling into slimcc-pir callees */
int clang_call_slimcc_callees(void);

#endif
