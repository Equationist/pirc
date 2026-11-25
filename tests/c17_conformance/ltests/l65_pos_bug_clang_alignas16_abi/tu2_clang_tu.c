/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: verifies bidirectional ABI compatibility with slimcc-pir
 * for _Alignas(16) mixed [INTEGER, SSE] structs, _Alignas(16) HFAs (both fixed
 * stack-spilled and variadic), and _Alignas(32) structs on ARM64 & x86-64.
 */
#include <stdarg.h>
#include "interop.h"

double clang_take_a16_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                            int i7, int i8, int stk1, A16Mix s) {
    return (double)(i1 + i2 + i3 + i4 + i5 + i6 + i7 + i8 + stk1 + s.a) + s.b;
}

float clang_take_hfa16_stack(float f0, float f1, float f2, float f3,
                             float f4, float f5, float f6, float f7,
                             int i0, int i1, int i2, int i3,
                             int i4, int i5, int i6, int i7,
                             int stk1, A16Hfa h) {
    return f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 +
           (float)(i0 + i1 + i2 + i3 + i4 + i5 + i6 + i7 + stk1) +
           h.v[0] + h.v[1] + h.v[2] + h.v[3];
}

double clang_take_a32_stack(int i1, int i2, int i3, int i4, int i5, int i6,
                            int i7, int i8, int stk1, A32Big b) {
    return (double)(i1 + i2 + i3 + i4 + i5 + i6 + i7 + i8 + stk1 + b.a) + b.b;
}

double clang_va_alignas16(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int k = va_arg(ap, int);
    Odd3 o3 = va_arg(ap, Odd3);
    A16Mix am = va_arg(ap, A16Mix);
    Odd9 o9 = va_arg(ap, Odd9);
    A16Hfa ah = va_arg(ap, A16Hfa);
    Float3 f3 = va_arg(ap, Float3);
    A32Big a32 = va_arg(ap, A32Big);
    va_end(ap);
    return (double)(n + k + o3.a[2] + am.a + o9.a[8] + a32.a) +
           am.b + (double)(ah.v[0] + ah.v[1] + ah.v[2] + ah.v[3]) +
           (double)(f3.x + f3.y + f3.z) + a32.b;
}

int clang_call_slimcc_callees(void) {
    A16Mix am = { 50, 0.25 };
    A16Hfa ah = {{ 1.0f, 2.0f, 3.0f, 4.0f }};
    A32Big a32 = { 70, 0.5 };
    Odd3 o3 = {{ 1, 2, 3 }};
    Odd9 o9 = {{ 1, 2, 3, 4, 5, 6, 7, 8, 9 }};
    Float3 f3 = { 0.5f, 1.5f, 2.0f };

    double r1 = slimcc_take_a16_stack(1, 2, 3, 4, 5, 6, 7, 8, 9, am);
    float  r2 = slimcc_take_hfa16_stack(1, 1, 1, 1, 1, 1, 1, 1,
                                        1, 1, 1, 1, 1, 1, 1, 1, 2, ah);
    double r3 = slimcc_take_a32_stack(1, 2, 3, 4, 5, 6, 7, 8, 9, a32);
    double r4 = slimcc_va_alignas16(7, 11, o3, am, o9, ah, f3, a32);

    if (r1 != 95.25) return 1;
    if (r2 != 28.0f) return 2;
    if (r3 != 115.5) return 3;
    if (r4 != 164.75) return 4;
    return 0;
}
