/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.3 / §6.9.2 — Classic C frontend tag-scope &
 * declaration edge cases (TCC, chibicc, cproc, kefir, GCC, Clang):
 *   - Tentative definition completed later in same TU (`int g_t; int g_t = 42; int g_t;`)
 *   - Nested `struct Inner` inside `struct Outer` is visible in ENCLOSING scope!
 *   - Block-scope forward tag `struct Tag;` shadows outer `struct Tag`!
 *   - Trailing comma in enum list, negative enum constants, INT_MIN/INT_MAX enums
 *   - Pointer-to-array `int (*p)[4]` arithmetic & 3D array decay
 */
#include <limits.h>
#include "c17_test.h"

/* Multiple tentative definitions + one real definition in same TU (C17 §6.9.2) */
int g_tent_val;
int g_tent_val = 42;
int g_tent_val;

extern int g_tent_arr[];
int g_tent_arr[4] = { 10, 20, 30, 40 };

/* Nested struct definition leaks tag `Inner` into file scope (C17 §6.2.1 p4) */
struct Outer {
    struct Inner {
        int val;
    } in;
    int extra;
};

/* Outer tag to be shadowed by block-scope forward declaration */
struct ShadowTag {
    int x;
};

/* Trailing comma + negative & extreme enum constants */
enum RangeEnum {
    E_MIN = INT_MIN,
    E_NEG = -5,
    E_NEXT,          /* -4 */
    E_MAX = INT_MAX, /* trailing comma valid in C99/C17 */
};

static int sum_3d(int a[][2][3], int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 2; j++) {
            for (int k = 0; k < 3; k++) {
                s += a[i][j][k];
            }
        }
    }
    return s;
}

int main(void) {
    C17_BEGIN("c67_bug_c17_decl_and_scope_traps",
              "Tentative defs, nested struct scope, forward tag shadow, enum, 3D array");

    C17_CHECK_EQ(g_tent_val, 42);
    C17_CHECK_EQ(g_tent_arr[3], 40);

    /* `struct Inner` defined inside `struct Outer` is directly usable here */
    {
        struct Inner i = { 99 };
        struct Outer o = { { 11 }, 22 };
        C17_CHECK_EQ(i.val + o.in.val + o.extra, 132);
    }

    /* Forward tag `struct ShadowTag;` shadows file-scope `struct ShadowTag` */
    {
        struct ShadowTag outer_s = { 7 };
        C17_CHECK_EQ(sizeof(outer_s), sizeof(int));
        {
            struct ShadowTag; /* NEW distinct incomplete tag in inner block! */
            struct Holder {
                struct ShadowTag *ptr;
            };
            struct ShadowTag {
                double d1;
                double d2;
            };
            struct ShadowTag inner_s = { 1.5, 2.5 };
            struct Holder h = { &inner_s };
            C17_CHECK_EQ(sizeof(*h.ptr), 2 * sizeof(double));
            C17_CHECK_DBL(h.ptr->d1 + h.ptr->d2, 4.0, 0.0);
        }
        C17_CHECK_EQ(outer_s.x, 7);
    }

    /* Enum constants */
    C17_CHECK_EQ(E_MIN, INT_MIN);
    C17_CHECK_EQ(E_NEG, -5);
    C17_CHECK_EQ(E_NEXT, -4);
    C17_CHECK_EQ(E_MAX, INT_MAX);

    /* Pointer to array `int (*p)[4]` & 3D array decay */
    {
        int mat[3][4] = {
            { 1, 2, 3, 4 },
            { 5, 6, 7, 8 },
            { 9, 10, 11, 12 }
        };
        int (*p)[4] = mat;
        C17_CHECK_EQ(sizeof(*p), 4 * sizeof(int));
        p += 2;
        C17_CHECK_EQ((*p)[3], 12);
        C17_CHECK_EQ(p - mat, 2);

        int cube[2][2][3] = {
            { { 1, 2, 3 }, { 4, 5, 6 } },
            { { 7, 8, 9 }, { 10, 11, 12 } }
        };
        C17_CHECK_EQ(sum_3d(cube, c17_opaque_int(2)), 78);
    }

    C17_END();
}
