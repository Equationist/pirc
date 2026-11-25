/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.2.7 / §6.5.15 / §6.6 / §6.7.6.3 / §6.7.9 —
 * Regression tests for:
 *   1. 64-bit static/global bitfield initialization (1ULL << 64 UB fix).
 *   2. Designated initializer overriding field of expression-initialized struct.
 *   3. Designated initializer switching active union member (auto & static).
 *   4. Composite type formation for redeclared pointer/array to incomplete array.
 *   5. Conditional operator qualifier merging on pointer-to-pointer.
 *   6. K&R definition with short/float matching promoted int/double prototype.
 *   7. const-qualified return type preservation vs unqualified call result.
 *   8. Short-circuit dead-branch avoidance in integer constant expressions.
 */
#include <stdint.h>
#include "c17_test.h"

/* 1. 64-bit static bitfield init */
struct BF64 {
    uint64_t u64 : 64;
    int64_t  s64 : 64;
    _Bool    b1  : 1;
};
static struct BF64 g_bf64 = {
    .u64 = 0x123456789abcdef0ULL,
    .s64 = -0x123456789abcdefLL,
    .b1  = 2
};

/* 2. Designated init overriding INIT_EXPR struct */
struct PairS { int a, b; };
struct WrapS { struct PairS s; int c; };
static struct PairS make_pair(void) { return (struct PairS){10, 20}; }

/* 3. Designated init switching active union member */
union SwU {
    struct { int a, b; } s;
    int x;
};

/* 4. Composite type across redeclarations */
extern int (*g_p)[];
extern int (*g_p)[4];
static int g_row[4] = {10, 20, 30, 40};
int (*g_p)[4] = &g_row;
_Static_assert(sizeof(*g_p) == 4 * sizeof(int), "composite ptr-to-array");

extern int (*g_arr[2])[];
extern int (*g_arr[2])[4];
int (*g_arr[2])[4] = { &g_row, &g_row };
_Static_assert(sizeof(*g_arr[0]) == 4 * sizeof(int), "composite array-of-ptr-to-array");

/* 6. K&R definition matching promoted prototype */
static int kr_short(int a);
static int kr_short(a)
    short a;
{
    return a + 1;
}

static int kr_float(double x);
static int kr_float(x)
    float x;
{
    return (int)(x * 2.0f);
}

/* 7. const return type preservation */
static const int const_ret_fn(void);
static const int const_ret_fn(void) { return 7; }

/* 8. Short-circuit ICE dead branches */
static int g_a[1 ? 4 : 1 / 0];
static int g_b[(0 && (1 / 0)) ? 1 : 4];
static int g_c[(1 || (1 / 0)) ? 4 : 1];
static int g_d[0 ? (1, 2) : 4];
static int *g_np = (1 ? 0 : 1 / 0);
_Static_assert(sizeof(g_a) == 4 * sizeof(int), "ternary short-circuit ICE");
_Static_assert(sizeof(g_b) == 4 * sizeof(int), "&& short-circuit ICE");
_Static_assert(sizeof(g_c) == 4 * sizeof(int), "|| short-circuit ICE");
_Static_assert(sizeof(g_d) == 4 * sizeof(int), "comma in dead branch ICE");

int main(void) {
    C17_BEGIN("c67_bug_bitfield64_desig_init_composite_and_kr",
              "64b static bitfield, desig init overrides, composite types, K&R promo, ICE short-circuit");

    /* 1 */
    C17_CHECK_EQ(g_bf64.u64, 0x123456789abcdef0ULL);
    C17_CHECK_EQ(g_bf64.s64, -0x123456789abcdefLL);
    C17_CHECK_EQ(g_bf64.b1, 1);

    /* 2 */
    struct WrapS w = { .s = make_pair(), .s.a = 3, .c = 99 };
    C17_CHECK_EQ(w.s.a, 3);
    C17_CHECK_EQ(w.s.b, 20);
    C17_CHECK_EQ(w.c, 99);

    /* 3 */
    union SwU u = { .s.a = 1, .x = 2, .s.b = 3 };
    C17_CHECK_EQ(u.s.a, 0);
    C17_CHECK_EQ(u.s.b, 3);
    static union SwU su = { .s.a = 1, .x = 2, .s.b = 3 };
    C17_CHECK_EQ(su.s.a, 0);
    C17_CHECK_EQ(su.s.b, 3);

    /* 4 */
    C17_CHECK_EQ((*g_p)[3], 40);
    C17_CHECK_EQ((*g_arr[0])[2], 30);

    /* 5. Conditional pointer qualifier merging */
    int xv = 1;
    int *xp = &xv;
    int * const *p1 = &xp;
    int * volatile *p2 = &xp;
    int cond = 1;
    _Static_assert(
        _Generic(cond ? p1 : p2, int * const volatile *: 1, default: 0) == 1,
        "conditional merges pointed-to pointer qualifiers"
    );
    C17_CHECK(*(cond ? p1 : p2) == xp);

    /* 6 */
    C17_CHECK_EQ(kr_short(41), 42);
    C17_CHECK_EQ(kr_float(3.5), 7);

    /* 7 */
    _Static_assert(_Generic(&const_ret_fn, const int (*)(void): 1, int (*)(void): 2, default: 3) == 1,
                   "function pointer preserves const return");
    _Static_assert(_Generic(const_ret_fn(), int: 1, const int: 2, default: 3) == 1,
                   "call expression drops return qualifiers");
    C17_CHECK_EQ(const_ret_fn(), 7);

    /* 8 */
    C17_CHECK(g_np == 0);

    C17_END();
}
