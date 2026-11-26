/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "interop.h"
#include <stdint.h>

long clang_take_ubf(long a, struct ubf_prefix u4, long b, struct ubf_hi_pad u8, long c) {
    return a * 1000 + (long)u4.x * 100 + b * 10 + (long)u8.y + c;
}

double clang_take_union_ubf_hfa(union u_hfa_bf1 u1, union u_hfa_bf2 u2, double d) {
    return (double)u1.f + (double)u2.p.x + (double)u2.p.y + d;
}

union u_hfa_bf2 clang_ret_union_ubf_hfa(float x, float y) {
    union u_hfa_bf2 u;
    u.p.x = x;
    u.p.y = y;
    return u;
}

struct hfa32 clang_ret_hfa32(double a, double b) {
    struct hfa32 h;
    h.a = a;
    h.b = b;
    return h;
}

double clang_take_hfa32(struct hfa32 h, double c) {
    if (((uintptr_t)&h & 31u) != 0)
        return -9999.0;
    return h.a + h.b + c;
}

int clang_call_slimcc_checks(void) {
    struct ubf_prefix u4 = { 5 };
    struct ubf_hi_pad u8 = { 3 };
    if (slimcc_take_ubf(7, u4, 8, u8, 9) != 7592)
        return 1;

    union u_hfa_bf1 h1;
    h1.f = 2.5f;
    union u_hfa_bf2 h2;
    h2.p.x = 3.5f;
    h2.p.y = 4.5f;
    if (slimcc_take_union_ubf_hfa(h1, h2, 5.5) != 16.0)
        return 2;

    union u_hfa_bf2 rh2 = slimcc_ret_union_ubf_hfa(8.25f, 9.75f);
    if (rh2.p.x != 8.25f || rh2.p.y != 9.75f)
        return 3;

    struct hfa32 rh32 = slimcc_ret_hfa32(11.5, 22.5);
    if (((uintptr_t)&rh32 & 31u) != 0)
        return 4;
    if (rh32.a != 11.5 || rh32.b != 22.5)
        return 5;
    if (slimcc_take_hfa32(rh32, 6.0) != 40.0)
        return 6;

    return 0;
}
