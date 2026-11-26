/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6 / §6.7.8 — Round 3 regressions:
 *   1. Parenthesized declarators shadowing typedef names:
 *        int (T) = 42;  struct S { int (T); };
 *      while keeping abstract parameter declarators working:
 *        int apply_fn(int (T));  // takes int (*)(T)
 *   2. K&R empty-paren definition matching (void) prototype
 *   3. Composite type preservation across prototype + definition
 *   4. Function typedef redeclaration with top-level parameter qualifiers
 */
#include "c17_test.h"

typedef int T;

/* In a parameter list, `int (T)` is an abstract declarator for `int (*)(T)` */
static int apply_fn(int (T));
static int apply_fn(int (*fn)(T)) {
    return fn(19);
}

static int dbl_t(T v) {
    return v * 2;
}

/* Struct member shadowing typedef via parenthesized declarator */
struct ShadowRec {
    int (T);
    int b;
};

/* Composite array parameter type across prototype & definition */
static int (*ret_row_ptr(int (*p)[4]))[];
static int (*ret_row_ptr(int (*p)[4]))[4] {
    return p;
}

/* Function typedef redeclaration with top-level param qualifiers (C17 §6.7.6.3p15) */
typedef int FnQual(int);
typedef int FnQual(const int);
static FnQual dbl_t;

int main(void) {
    C17_BEGIN("c67_bug_r3_paren_typedef_shadow_and_proto_composite",
              "Paren declarator typedef shadowing, K&R (void) match, composite proto, fn typedef quals");

    /* 1. Block-scope parenthesized declarator shadowing typedef T */
    {
        int (T) = 42;
        C17_CHECK_EQ(T, 42);
        T += 8;
        C17_CHECK_EQ(T, 50);
    }

    /* Typedef T is visible again outside the inner block */
    T outer_val = 15;
    C17_CHECK_EQ(outer_val, 15);
    C17_CHECK_EQ(apply_fn(dbl_t), 38);

    /* Struct member named T */
    struct ShadowRec sr = { 123, 456 };
    C17_CHECK_EQ(sr.T, 123);
    C17_CHECK_EQ(sr.b, 456);

    /* 2. Composite return type preserved */
    int row[4] = { 10, 20, 30, 40 };
    int (*rp)[4] = ret_row_ptr(&row);
    C17_CHECK_EQ(sizeof(*rp), 4 * sizeof(int));
    C17_CHECK_EQ((*rp)[3], 40);

    /* 4. FnQual typedef usable as function pointer */
    FnQual *fp = dbl_t;
    C17_CHECK_EQ(fp(21), 42);

    C17_END();
}
