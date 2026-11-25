/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) — Bidirectional slimcc-pir <-> clang AAPCS64
 * union-HFA, over-aligned HFA, and stack-spilled small struct ABI interop.
 */
#include "c17_test.h"
#include "interop.h"

double slimcc_consume_union_hfas(union hfa_f1 u1, union hfa_f2 u2,
                                 union hfa_f2_mix u3, union hfa_d3 u4,
                                 union non_hfa_fi u5) {
    return (double)u1.a + (double)(u2.p.x + u2.p.y)
         + (double)(u3.pair.x + u3.pair.y)
         + (u4.arr[0] + u4.arr[1] + u4.arr[2])
         + (double)u5.i;
}

float slimcc_consume_nested_and_over(struct with_union_hfa w, struct over_hfa16 o) {
    return w.z + w.u.pair.x + w.u.pair.y + o.a + o.b + o.c + o.d;
}

union hfa_f2 slimcc_return_union_hfa(float x, float y) {
    union hfa_f2 u;
    u.p.x = x;
    u.p.y = y;
    return u;
}

struct over_hfa16 slimcc_return_over_hfa(float a, float b, float c, float d) {
    struct over_hfa16 o = { a, b, c, d };
    return o;
}

int slimcc_consume_spilled_structs(long g0, long g1, long g2, long g3,
                                   long g4, long g5, long g6, long g7,
                                   struct stk_s3 s1, struct stk_s3 s2,
                                   struct stk_s12 s3) {
    return (int)(g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7)
         + (int)s1.a + (int)s1.b + (int)s1.c
         + (int)s2.a + (int)s2.b + (int)s2.c
         + s3.a + s3.b + s3.c;
}

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_union_hfa",
              "Bidirectional slimcc-pir <-> clang union-HFA & over-aligned HFA ABI");

    /* Direction 1: slimcc-pir caller -> clang callee */
    {
        union hfa_f1 u1; u1.a = 1.5f;
        union hfa_f2 u2; u2.p.x = 2.5f; u2.p.y = 3.0f;
        union hfa_f2_mix u3; u3.pair.x = 4.0f; u3.pair.y = 5.0f;
        union hfa_d3 u4; u4.arr[0] = 10.0; u4.arr[1] = 20.0; u4.arr[2] = 30.0;
        union non_hfa_fi u5; u5.i = 7;
        /* 1.5 + 5.5 + 9.0 + 60.0 + 7.0 = 83.0 */
        double r1 = clang_consume_union_hfas(u1, u2, u3, u4, u5);
        C17_CHECK_EQ((int)r1, 83);

        struct with_union_hfa w;
        w.z = 1.0f; w.u.pair.x = 2.0f; w.u.pair.y = 3.0f;
        struct over_hfa16 o = { 4.0f, 5.0f, 6.0f, 7.0f };
        /* 1+2+3+4+5+6+7 = 28.0 */
        float r2 = clang_consume_nested_and_over(w, o);
        C17_CHECK_EQ((int)r2, 28);

        union hfa_f2 ru = clang_return_union_hfa(11.0f, 13.0f);
        C17_CHECK_EQ((int)(ru.p.x + ru.p.y), 24);

        struct over_hfa16 ro = clang_return_over_hfa(2.0f, 4.0f, 6.0f, 8.0f);
        C17_CHECK_EQ((int)(ro.a + ro.b + ro.c + ro.d), 20);

        struct stk_s3 s1 = { 1, -2, 3 };
        struct stk_s3 s2 = { 4, 5, -6 };
        struct stk_s12 s3 = { 10, 20, 30 };
        /* (0..7 => 28) + (1-2+3 => 2) + (4+5-6 => 3) + (10+20+30 => 60) = 93 */
        int r3 = clang_consume_spilled_structs(0, 1, 2, 3, 4, 5, 6, 7, s1, s2, s3);
        C17_CHECK_EQ(r3, 93);
    }

    /* Direction 2: clang caller -> slimcc-pir callee */
    C17_CHECK_EQ(clang_verify_slimcc_union_hfas(), 0);

    C17_END();
}
