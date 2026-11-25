/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1 — Real-world _Generic edge-case bugs
 * (GCC, Clang, chibicc, kefir, DR 481):
 *   - Lvalue conversion strips top-level const, volatile, _Atomic, restrict
 *   - Pointed-to qualifiers (const int *) are PRESERVED and distinct from int *
 *   - Array-to-pointer decay & function-to-pointer decay in controlling expr
 *   - Controlling expression is NOT evaluated (side effects must not execute)
 *   - Nested _Generic selections
 */
#include "c17_test.h"

static int sample_fn(int x) { return x + 1; }

#define KIND_OF(x) _Generic((x), \
    int: 1, \
    unsigned int: 2, \
    double: 3, \
    int *: 4, \
    const int *: 5, \
    int (*)(int): 6, \
    default: 99)

int main(void) {
    C17_BEGIN("c65_bug_generic_edge_cases",
              "_Generic DR 481 qual stripping, array/fn decay, non-eval, nesting");

    const int ci = 10;
    volatile int vi = 20;
    int arr[4] = { 1, 2, 3, 4 };
    int *restrict rp = arr;
    const int *cp = arr;
    int side = 0;

    C17_CHECK_EQ(KIND_OF(ci), 1);
    C17_CHECK_EQ(KIND_OF(vi), 1);
    C17_CHECK_EQ(KIND_OF(arr), 4);       /* int[4] decays to int * */
    C17_CHECK_EQ(KIND_OF(rp), 4);        /* int *restrict drops top-level restrict */
    C17_CHECK_EQ(KIND_OF(cp), 5);        /* const int * preserves pointed-to const */
    C17_CHECK_EQ(KIND_OF(sample_fn), 6); /* function designator decays to fn ptr */
    C17_CHECK_EQ(KIND_OF("hello"), 99);  /* char[6] decays to char * -> default */
    C17_CHECK_EQ(KIND_OF(3.14), 3);

#ifndef __STDC_NO_ATOMICS__
    _Atomic int ai = 5;
    C17_CHECK_EQ(KIND_OF(ai), 1);        /* C17 DR 481 strips _Atomic */
#endif

    /* Controlling expression MUST NOT be evaluated */
    int k = KIND_OF(side++);
    C17_CHECK_EQ(k, 1);
    C17_CHECK_EQ(side, 0);

    /* Nested _Generic */
    int nested = _Generic(ci,
        int: _Generic(cp, const int *: 42, default: 0),
        default: -1);
    C17_CHECK_EQ(nested, 42);

    C17_END();
}
