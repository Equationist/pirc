/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9 — Real-world small-compiler static initializer bugs
 * (TCC, chibicc, cproc, kefir):
 *   - Exact-fit string literal initializer char s[4] = "abcd" (no NUL terminator)
 *   - 2D char array initialized from string literals (exact-fit & short)
 *   - Self-referential and mutually-referential static struct pointers
 *   - Static array of function pointers initialized with both `fn` and `&fn`
 *   - File-scope compound literal pointer in static initializer
 */
#include <stddef.h>
#include "c17_test.h"

/* Exact-fit string literal without NUL terminator (C17 §6.7.9 p14) */
static const char g_exact4[4] = "abcd";
static const char g_grid[3][4] = { "ABCD", "XY", "" };

/* Mutual forward pointers in static struct initializers */
struct cyc_node {
    const struct cyc_node *next;
    int val;
};

static const struct cyc_node g_node_b;
static const struct cyc_node g_node_a = { &g_node_b, 100 };
static const struct cyc_node g_node_b = { &g_node_a, 200 };
static const struct cyc_node g_self   = { &g_self,   300 };

/* Static array of function pointers (both bare name and &name) */
static int fn_add1(int x) { return x + 1; }
static int fn_mul2(int x) { return x * 2; }
static int fn_sub3(int x) { return x - 3; }

static int (*const g_fn_tbl[3])(int) = { fn_add1, &fn_mul2, fn_sub3 };

/* File-scope compound literal with static storage duration (C17 §6.5.2.5 p5) */
static const int *g_cl_ptr = (const int[]){ 11, 22, 33, 44 };

int main(void) {
    C17_BEGIN("c67_bug_static_inits_smallcc",
              "Exact-fit strings, cyclic static ptrs, fnptr table, file-scope complit");

    /* Exact-fit 4-char array (sizeof == 4, no NUL) */
    C17_CHECK_EQ(sizeof(g_exact4), 4);
    C17_CHECK_EQ(g_exact4[0], 'a');
    C17_CHECK_EQ(g_exact4[1], 'b');
    C17_CHECK_EQ(g_exact4[2], 'c');
    C17_CHECK_EQ(g_exact4[3], 'd');

    /* 2D char array */
    C17_CHECK_EQ(g_grid[0][0], 'A');
    C17_CHECK_EQ(g_grid[0][3], 'D');
    C17_CHECK_EQ(g_grid[1][0], 'X');
    C17_CHECK_EQ(g_grid[1][1], 'Y');
    C17_CHECK_EQ(g_grid[1][2], '\0');
    C17_CHECK_EQ(g_grid[1][3], '\0');
    C17_CHECK_EQ(g_grid[2][0], '\0');

    /* Local exact-fit string literal initializer too */
    {
        char loc[3] = "xyz";
        C17_CHECK_EQ(sizeof(loc), 3);
        C17_CHECK_EQ(loc[0], 'x');
        C17_CHECK_EQ(loc[1], 'y');
        C17_CHECK_EQ(loc[2], 'z');
    }

    /* Cyclic & self-referential static struct pointers */
    C17_CHECK(g_node_a.next == &g_node_b);
    C17_CHECK(g_node_b.next == &g_node_a);
    C17_CHECK_EQ(g_node_a.next->next->val, 100);
    C17_CHECK_EQ(g_node_b.next->next->val, 200);
    C17_CHECK(g_self.next == &g_self);
    C17_CHECK_EQ(g_self.next->next->val, 300);

    /* Function pointer table */
    {
        int x = c17_opaque_int(10);
        C17_CHECK_EQ(g_fn_tbl[0](x), 11);
        C17_CHECK_EQ(g_fn_tbl[1](x), 20);
        C17_CHECK_EQ(g_fn_tbl[2](x), 7);
    }

    /* File-scope compound literal */
    C17_CHECK_EQ(g_cl_ptr[0], 11);
    C17_CHECK_EQ(g_cl_ptr[3], 44);

    C17_END();
}
