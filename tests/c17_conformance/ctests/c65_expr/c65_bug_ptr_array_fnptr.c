/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.1 / §6.5.3.2 / §6.5.6 — Real-world pointer &
 * function-pointer bugs:
 *   - Pointer subtraction p - q on non-power-of-two struct size (sizeof == 13)
 *     for both positive and negative differences
 *   - Commutative subscripting i[arr] and negative index (-1)[p]
 *   - Pointer to function returning pointer to array: int (*(*get_fn(int))(int))[3]
 *   - Multi-dereference of function pointers (***fp)(x) and (&fn)(x)
 *   - Null pointer comparisons for object & function pointers
 */
#include <stddef.h>
#include "c17_test.h"

struct odd13 {
    char data[13];
};

static int g_row0[3] = { 10, 20, 30 };
static int g_row1[3] = { 40, 50, 60 };

static int (*pick_row_a(int idx))[3] {
    return (idx == 0) ? &g_row0 : &g_row1;
}

static int (*pick_row_b(int idx))[3] {
    return (idx == 0) ? &g_row1 : &g_row0;
}

/* Function returning pointer to function returning pointer to int[3] */
static int (*(*get_picker(int which))(int))[3] {
    return (which == 0) ? pick_row_a : pick_row_b;
}

static int triple_val(int x) {
    return x * 3;
}

int main(void) {
    C17_BEGIN("c65_bug_ptr_array_fnptr",
              "Non-pow2 ptr diff (13B), i[arr], fn->ptr->arr[3], (***fp)(x), null fnptr");

    /* Non-power-of-two (13-byte) pointer subtraction positive & negative */
    {
        C17_CHECK_EQ(sizeof(struct odd13), 13);
        struct odd13 arr[10];
        int i = c17_opaque_int(2);
        int j = c17_opaque_int(9);
        struct odd13 *p = &arr[i];
        struct odd13 *q = &arr[j];
        ptrdiff_t d_pos = q - p;
        ptrdiff_t d_neg = p - q;
        ptrdiff_t d_zero = p - p;
        C17_CHECK_EQ(d_pos, 7);
        C17_CHECK_EQ(d_neg, -7);
        C17_CHECK_EQ(d_zero, 0);
        C17_CHECK(p + 7 == q);
        C17_CHECK(q - 7 == p);
    }

    /* Commutative subscripting & negative indexing */
    {
        int nums[5] = { 11, 22, 33, 44, 55 };
        int idx = c17_opaque_int(3);
        C17_CHECK_EQ(idx[nums], 44);
        int *mid = &nums[2];
        C17_CHECK_EQ((-1)[mid], 22);
        C17_CHECK_EQ((-2)[mid], 11);
        C17_CHECK_EQ(mid[-2], 11);
    }

    /* Complex declarator: int (*(*get_picker(int))(int))[3] */
    {
        int (*row_a)[3] = get_picker(c17_opaque_int(0))(c17_opaque_int(1));
        int (*row_b)[3] = get_picker(c17_opaque_int(1))(c17_opaque_int(1));
        C17_CHECK_EQ((*row_a)[2], 60);
        C17_CHECK_EQ((*row_b)[1], 20);
    }

    /* Arbitrary dereferencing of function designator / pointer (C17 §6.5.3.2 p4) */
    {
        int (*fp)(int) = triple_val;
        int x = c17_opaque_int(14);
        C17_CHECK_EQ(fp(x), 42);
        C17_CHECK_EQ((*fp)(x), 42);
        C17_CHECK_EQ((***fp)(x), 42);
        C17_CHECK_EQ((&triple_val)(x), 42);
        C17_CHECK_EQ((**triple_val)(x), 42);
    }

    /* Null pointer comparisons (object & function pointers) */
    {
        int (*fp_null)(int) = NULL;
        int (*fp_zero)(int) = 0;
        int (*fp_live)(int) = triple_val;
        C17_CHECK(fp_null == NULL);
        C17_CHECK(fp_null == 0);
        C17_CHECK(fp_null == fp_zero);
        C17_CHECK(!fp_null);
        C17_CHECK(fp_live != NULL);
        C17_CHECK(fp_live != fp_null);
        C17_CHECK(fp_live ? 1 : 0);
    }

    C17_END();
}
