/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: verifies true binary ABI compatibility with slimcc-pir
 * for AAPCS64 union HFAs, over-aligned HFAs, and stack-spilled structs.
 */
#include "interop.h"

double clang_consume_union_hfas(union hfa_f1 u1, union hfa_f2 u2,
                                union hfa_f2_mix u3, union hfa_d3 u4,
                                union non_hfa_fi u5) {
    return (double)u1.a + (double)(u2.p.x + u2.p.y)
         + (double)(u3.pair.x + u3.pair.y)
         + (u4.arr[0] + u4.arr[1] + u4.arr[2])
         + (double)u5.i;
}

float clang_consume_nested_and_over(struct with_union_hfa w, struct over_hfa16 o) {
    return w.z + w.u.pair.x + w.u.pair.y + o.a + o.b + o.c + o.d;
}

union hfa_f2 clang_return_union_hfa(float x, float y) {
    union hfa_f2 u;
    u.p.x = x;
    u.p.y = y;
    return u;
}

struct over_hfa16 clang_return_over_hfa(float a, float b, float c, float d) {
    struct over_hfa16 o = { a, b, c, d };
    return o;
}

int clang_consume_spilled_structs(long g0, long g1, long g2, long g3,
                                  long g4, long g5, long g6, long g7,
                                  struct stk_s3 s1, struct stk_s3 s2,
                                  struct stk_s12 s3) {
    return (int)(g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7)
         + (int)s1.a + (int)s1.b + (int)s1.c
         + (int)s2.a + (int)s2.b + (int)s2.c
         + s3.a + s3.b + s3.c;
}

int clang_verify_slimcc_union_hfas(void) {
    union hfa_f1 u1; u1.a = 1.5f;
    union hfa_f2 u2; u2.p.x = 2.5f; u2.p.y = 3.0f;
    union hfa_f2_mix u3; u3.pair.x = 4.0f; u3.pair.y = 5.0f;
    union hfa_d3 u4; u4.arr[0] = 10.0; u4.arr[1] = 20.0; u4.arr[2] = 30.0;
    union non_hfa_fi u5; u5.i = 7;
    if ((int)slimcc_consume_union_hfas(u1, u2, u3, u4, u5) != 83) return 1;

    struct with_union_hfa w;
    w.z = 1.0f; w.u.pair.x = 2.0f; w.u.pair.y = 3.0f;
    struct over_hfa16 o = { 4.0f, 5.0f, 6.0f, 7.0f };
    if ((int)slimcc_consume_nested_and_over(w, o) != 28) return 2;

    union hfa_f2 ru = slimcc_return_union_hfa(11.0f, 13.0f);
    if ((int)(ru.p.x + ru.p.y) != 24) return 3;

    struct over_hfa16 ro = slimcc_return_over_hfa(2.0f, 4.0f, 6.0f, 8.0f);
    if ((int)(ro.a + ro.b + ro.c + ro.d) != 20) return 4;

    struct stk_s3 s1 = { 1, -2, 3 };
    struct stk_s3 s2 = { 4, 5, -6 };
    struct stk_s12 s3 = { 10, 20, 30 };
    if (slimcc_consume_spilled_structs(0, 1, 2, 3, 4, 5, 6, 7, s1, s2, s3) != 93) return 5;

    return 0;
}
