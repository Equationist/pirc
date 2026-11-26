/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) — Bidirectional slimcc-pir <-> clang ABI interop
 * for unnamed-bitfield-padded structs, union-with-unnamed-bf HFAs, and
 * _Alignas(32) HFA returns/arguments.
 */
#include "c17_test.h"
#include "interop.h"
#include <stdint.h>

long slimcc_take_ubf(long a, struct ubf_prefix u4, long b, struct ubf_hi_pad u8, long c) {
    return a * 1000 + (long)u4.x * 100 + b * 10 + (long)u8.y + c;
}

double slimcc_take_union_ubf_hfa(union u_hfa_bf1 u1, union u_hfa_bf2 u2, double d) {
    return (double)u1.f + (double)u2.p.x + (double)u2.p.y + d;
}

union u_hfa_bf2 slimcc_ret_union_ubf_hfa(float x, float y) {
    union u_hfa_bf2 u;
    u.p.x = x;
    u.p.y = y;
    return u;
}

struct hfa32 slimcc_ret_hfa32(double a, double b) {
    struct hfa32 h;
    h.a = a;
    h.b = b;
    return h;
}

double slimcc_take_hfa32(struct hfa32 h, double c) {
    if (((uintptr_t)&h & 31u) != 0)
        return -9999.0;
    return h.a + h.b + c;
}

int main(void) {
    struct ubf_prefix u4 = { 2 };
    struct ubf_hi_pad u8 = { 6 };
    C17_CHECK(clang_take_ubf(3, u4, 4, u8, 5) == 3251);

    union u_hfa_bf1 h1;
    h1.f = 1.5f;
    union u_hfa_bf2 h2;
    h2.p.x = 2.25f;
    h2.p.y = 3.25f;
    C17_CHECK(clang_take_union_ubf_hfa(h1, h2, 4.0) == 11.0);

    union u_hfa_bf2 rh2 = clang_ret_union_ubf_hfa(6.5f, 7.5f);
    C17_CHECK(rh2.p.x == 6.5f && rh2.p.y == 7.5f);

    struct hfa32 rh32 = clang_ret_hfa32(10.5, 20.5);
    C17_CHECK(((uintptr_t)&rh32 & 31u) == 0);
    C17_CHECK(rh32.a == 10.5 && rh32.b == 20.5);
    C17_CHECK(clang_take_hfa32(rh32, 5.0) == 36.0);

    C17_CHECK(clang_call_slimcc_checks() == 0);
    C17_END();
}
