/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.13..§6.5.17 — Real-world evaluation order &
 * aggregate expression bugs:
 *   - Short-circuit && / || preventing side effects, div-by-0, and null deref
 *   - Comma operator returning small & large (>16B) structs by value
 *   - Ternary ?: returning small & large (>16B) structs by value, and void arms
 *   - Compound assignment lhs evaluated ONCE (array index, bitfield ptr, ptr-to-ptr)
 */
#include <stddef.h>
#include "c17_test.h"

struct small_s {
    int a;
    int b;
};

struct big_s {
    long long v[4];
};

static int g_log = 0;

static int bump_and_ret(int code, int ret) {
    g_log = g_log * 10 + code;
    return ret;
}

static void void_act(int code) {
    g_log = g_log * 10 + code;
}

struct bf_holder {
    unsigned f : 6;
};

static struct bf_holder g_bf;
static int g_ptr_calls = 0;

static struct bf_holder *get_bf_once(void) {
    g_ptr_calls++;
    return &g_bf;
}

static int g_arr[4] = { 10, 20, 30, 40 };
static int g_idx_calls = 0;

static int get_idx_once(void) {
    g_idx_calls++;
    return 2;
}

int main(void) {
    C17_BEGIN("c65_bug_eval_order_sideeffects",
              "Short-circuit guards, struct comma/ternary, void ternary, lhs eval-once");

    /* Short-circuit && / || must NOT evaluate RHS when short-circuited */
    {
        int d = c17_opaque_int(0);
        int *np = NULL;
        g_log = 0;
        int r1 = (d != 0) && ((100 / d) > 5);
        int r2 = (d == 0) || ((100 / d) > 5);
        int r3 = (np != NULL) && (*np == 1);
        int r4 = (np == NULL) || (*np == 1);
        int r5 = bump_and_ret(1, 0) && bump_and_ret(2, 1);
        int r6 = bump_and_ret(3, 1) || bump_and_ret(4, 1);
        C17_CHECK_EQ(r1, 0);
        C17_CHECK_EQ(r2, 1);
        C17_CHECK_EQ(r3, 0);
        C17_CHECK_EQ(r4, 1);
        C17_CHECK_EQ(r5, 0);
        C17_CHECK_EQ(r6, 1);
        C17_CHECK_EQ(g_log, 13); /* 2 and 4 must NOT have executed */
    }

    /* Comma operator returning small & large (>16B) structs */
    {
        struct small_s s1 = { 11, 22 };
        struct big_s b1 = { { 100, 200, 300, 400 } };
        g_log = 0;
        struct small_s rs = (void_act(1), void_act(2), s1);
        struct big_s rb = (void_act(3), b1);
        C17_CHECK_EQ(g_log, 123);
        C17_CHECK_EQ(rs.a, 11);
        C17_CHECK_EQ(rs.b, 22);
        C17_CHECK_EQ(rb.v[0], 100);
        C17_CHECK_EQ(rb.v[3], 400);
    }

    /* Ternary ?: returning small & large (>16B) structs, and void arms */
    {
        struct small_s sa = { 1, 2 }, sb = { 3, 4 };
        struct big_s ba = { { 10, 20, 30, 40 } };
        struct big_s bb = { { 50, 60, 70, 80 } };

        int cond_t = c17_opaque_int(1);
        int cond_f = c17_opaque_int(0);

        struct small_s r1 = cond_t ? sa : sb;
        struct small_s r2 = cond_f ? sa : sb;
        C17_CHECK_EQ(r1.a, 1);
        C17_CHECK_EQ(r1.b, 2);
        C17_CHECK_EQ(r2.a, 3);
        C17_CHECK_EQ(r2.b, 4);

        struct big_s r3 = cond_t ? ba : bb;
        struct big_s r4 = cond_f ? ba : bb;
        C17_CHECK_EQ(r3.v[0], 10);
        C17_CHECK_EQ(r3.v[3], 40);
        C17_CHECK_EQ(r4.v[0], 50);
        C17_CHECK_EQ(r4.v[3], 80);

        g_log = 0;
        cond_t ? void_act(5) : void_act(6);
        cond_f ? void_act(7) : void_act(8);
        C17_CHECK_EQ(g_log, 58);
    }

    /* Compound assignment LHS evaluated ONCE */
    {
        g_bf.f = 10;
        g_ptr_calls = 0;
        get_bf_once()->f += 7;
        C17_CHECK_EQ(g_ptr_calls, 1);
        C17_CHECK_EQ(g_bf.f, 17);

        g_idx_calls = 0;
        g_arr[get_idx_once()] *= 3;
        C17_CHECK_EQ(g_idx_calls, 1);
        C17_CHECK_EQ(g_arr[2], 90);
    }

    C17_END();
}
