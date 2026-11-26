/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.3 / §6.7.9 / §6.6 — Round 4 regressions:
 *   1. volatile struct read/write/copy, local brace-init, and comma expression
 *   2. Static designated bitfield override (clearing previous bits)
 *   3. Static _Bool from derived addresses and static pointer comparisons
 *   4. File-scope array initialized with compound literals (count_array_init_elements)
 *   5. Anonymous struct with _Alignas(16)
 *   6. Block-scope extern array bound without leaking to file scope
 */
#include "c17_test.h"
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct VolPair {
    int a;
    long long b;
};

struct BF {
    unsigned a : 4;
    unsigned b : 4;
    unsigned c : 8;
};

static struct BF g_bf = { .a = 0xf, .b = 0xa, .c = 0x55, .a = 0x2 };

static int g_arr[4] = { 10, 20, 30, 40 };
struct StaticSt {
    int x;
    int y;
};
static struct StaticSt g_st = { 1, 2 };

static _Bool b_arr_elem = (_Bool)(&g_arr[1]);
static _Bool b_st_mem = (_Bool)(&g_st.y);
static _Bool b_str_off = (_Bool)("hello" + 2);
static int cmp_ptr_lt = (&g_arr[0] < &g_arr[2]);
static int cmp_ptr_eq = (&g_arr[1] == &g_arr[1]);
static int cmp_ptr_ne_null = (&g_st != (struct StaticSt *)0);
static int cmp_ptr_eq_null = (&g_st == (struct StaticSt *)0);

static int g_complit_arr[] = { (int){ 11 }, (int){ 22 }, (int){ 33 } };

struct AnonAligned {
    char tag;
    _Alignas(16) struct {
        int vx;
        int vy;
    };
};

extern int g_scoped_ext[];
static int check_block_extern(void) {
    extern int g_scoped_ext[4];
    return (int)(sizeof(g_scoped_ext) / sizeof(g_scoped_ext[0])) + g_scoped_ext[3];
}
int g_scoped_ext[4] = { 1, 2, 3, 40 };

int main(void) {
    /* 1. volatile struct operations */
    volatile struct VolPair vp = { 7, 123456789LL };
    struct VolPair copy = vp;
    C17_CHECK(copy.a == 7);
    C17_CHECK(copy.b == 123456789LL);

    struct VolPair src = { 99, -42LL };
    vp = src;
    C17_CHECK(vp.a == 99);
    C17_CHECK(vp.b == -42LL);

    volatile struct VolPair *pvp = &vp;
    int from_comma = (0, *pvp).a;
    C17_CHECK(from_comma == 99);

    /* 2. Static designated bitfield override */
    C17_CHECK(g_bf.a == 0x2);
    C17_CHECK(g_bf.b == 0xa);
    C17_CHECK(g_bf.c == 0x55);

    /* 3. Static _Bool derived address & pointer comparisons */
    C17_CHECK(b_arr_elem == 1);
    C17_CHECK(b_st_mem == 1);
    C17_CHECK(b_str_off == 1);
    C17_CHECK(cmp_ptr_lt == 1);
    C17_CHECK(cmp_ptr_eq == 1);
    C17_CHECK(cmp_ptr_ne_null == 1);
    C17_CHECK(cmp_ptr_eq_null == 0);

    /* 4. File-scope compound literal array init */
    C17_CHECK(sizeof(g_complit_arr) / sizeof(g_complit_arr[0]) == 3);
    C17_CHECK(g_complit_arr[0] == 11 && g_complit_arr[1] == 22 && g_complit_arr[2] == 33);

    /* 5. Anonymous struct with _Alignas(16) */
    C17_CHECK(alignof(struct AnonAligned) == 16);
    C17_CHECK(offsetof(struct AnonAligned, vx) == 16);
    struct AnonAligned aa = { .tag = 'A', .vx = 5, .vy = 6 };
    C17_CHECK(aa.tag == 'A' && aa.vx == 5 && aa.vy == 6);

    /* 6. Block-scope extern */
    C17_CHECK(check_block_extern() == 44);

    C17_END();
}
