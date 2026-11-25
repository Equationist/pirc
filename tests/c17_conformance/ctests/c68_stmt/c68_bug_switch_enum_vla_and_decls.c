/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2, §6.5.2.5, §6.7.9 — Enum switch with
 * negative cases & fallthrough across nested blocks, compound literals of
 * structs-with-arrays in ternary & by-value calls, multi-dimensional
 * designated initializers, and shrink-wrap-eligible recursion with callee-saved
 * GP+FP state.
 */
#include <stdint.h>
#include "c17_test.h"

enum neg_kind {
    NK_NEG2 = -2,
    NK_NEG1 = -1,
    NK_ZERO = 0,
    NK_POS1 = 1,
    NK_POS2 = 2
};

static int switch_enum_nested(enum neg_kind k, int x) {
    int acc = 0;
    switch (k) {
    case NK_NEG2:
        acc += 100;
        {
            int local = x + 1;
            acc += local;
        }
        /* fall through */
    case NK_NEG1:
        acc += 20;
        break;
    case NK_ZERO:
        if (x > 0) {
            case NK_POS1:
            acc += x * 3;
        } else {
            acc -= x;
        }
        break;
    case NK_POS2:
        acc = 777;
        break;
    default:
        acc = -999;
        break;
    }
    return acc;
}

struct vec4 {
    int v[4];
};

static struct vec4 add_vec4(struct vec4 a, struct vec4 b) {
    struct vec4 r;
    for (int i = 0; i < 4; i++) r.v[i] = a.v[i] + b.v[i];
    return r;
}

static struct vec4 (*volatile g_add_vec4)(struct vec4, struct vec4) = add_vec4;

/* Shrink-wrap-eligible recursive function with 32-bit/64-bit signed args and
 * callee-saved GP + FP work on the slow path (exercises arm64_frame shrink-wrap). */
static int64_t shrinkwrap_rec(int32_t n, int64_t a, double scale) {
    if (n <= 0) {
        return a;
    }
    int64_t left = shrinkwrap_rec(n - 1, a + 1, scale);
    int64_t right = (n > 2) ? shrinkwrap_rec(n - 2, a - 1, scale * 0.5) : 0;
    return left + right + (int64_t)(scale * 2.0);
}

static int64_t (*volatile g_sw_rec)(int32_t, int64_t, double) = shrinkwrap_rec;

int main(void) {
    C17_BEGIN("c68_bug_switch_enum_vla_and_decls",
              "Negative-enum switch, compound literal ternary, 2D designators, shrink-wrap");

    /* 1. Negative-enum switch & jump into nested if-branch */
    C17_CHECK_EQ(switch_enum_nested((enum neg_kind)c17_opaque_int(-2), c17_opaque_int(5)), 126);
    C17_CHECK_EQ(switch_enum_nested((enum neg_kind)c17_opaque_int(-1), c17_opaque_int(5)), 20);
    C17_CHECK_EQ(switch_enum_nested((enum neg_kind)c17_opaque_int(0), c17_opaque_int(-4)), 4);
    C17_CHECK_EQ(switch_enum_nested((enum neg_kind)c17_opaque_int(1), c17_opaque_int(-4)), -12);
    C17_CHECK_EQ(switch_enum_nested((enum neg_kind)c17_opaque_int(2), c17_opaque_int(0)), 777);

    /* 2. Compound literal of struct-containing-array in ternary & call */
    {
        int cond = c17_opaque_int(1);
        struct vec4 r = g_add_vec4(
            cond ? (struct vec4){ { 1, 2, 3, 4 } } : (struct vec4){ { 9, 9, 9, 9 } },
            !cond ? (struct vec4){ { 8, 8, 8, 8 } } : (struct vec4){ { 10, 20, 30, 40 } }
        );
        C17_CHECK_EQ(r.v[0], 11);
        C17_CHECK_EQ(r.v[1], 22);
        C17_CHECK_EQ(r.v[2], 33);
        C17_CHECK_EQ(r.v[3], 44);
    }

    /* 3. Multi-dimensional designated initializers with out-of-order overrides */
    {
        int m[3][4] = {
            [1][2] = 7,
            [2] = { [0] = 9, 10, [3] = 13 },
            [0][3] = 5,
            [1][0] = 4,
            [1][2] = 70  /* override earlier [1][2] */
        };
        C17_CHECK_EQ(m[0][0], 0);
        C17_CHECK_EQ(m[0][3], 5);
        C17_CHECK_EQ(m[1][0], 4);
        C17_CHECK_EQ(m[1][1], 0);
        C17_CHECK_EQ(m[1][2], 70);
        C17_CHECK_EQ(m[1][3], 0);
        C17_CHECK_EQ(m[2][0], 9);
        C17_CHECK_EQ(m[2][1], 10);
        C17_CHECK_EQ(m[2][2], 0);
        C17_CHECK_EQ(m[2][3], 13);
    }

    /* 4. Shrink-wrap-eligible recursion with negative & positive n */
    {
        C17_CHECK_EQ(g_sw_rec(c17_opaque_int(-5), c17_opaque_int(42), c17_opaque_dbl(3.0)), 42);
        C17_CHECK_EQ(g_sw_rec(c17_opaque_int(0), c17_opaque_int(-7), c17_opaque_dbl(3.0)), -7);
        /* n=1, a=10, s=2.0: rec(0,11)+0+4 = 15
         * n=2, a=10, s=2.0: rec(1,11,2)+0+4 = (12+4)+4 = 20
         * n=3, a=10, s=2.0: rec(2,11,2) + rec(1,9,1) + 4 = (21) + (10+2) + 4 = 37 */
        C17_CHECK_EQ(g_sw_rec(c17_opaque_int(1), c17_opaque_int(10), c17_opaque_dbl(2.0)), 15);
        C17_CHECK_EQ(g_sw_rec(c17_opaque_int(2), c17_opaque_int(10), c17_opaque_dbl(2.0)), 20);
        C17_CHECK_EQ(g_sw_rec(c17_opaque_int(3), c17_opaque_int(10), c17_opaque_dbl(2.0)), 37);
    }

    C17_END();
}
