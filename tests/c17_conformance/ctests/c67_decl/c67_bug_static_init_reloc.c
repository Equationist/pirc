/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9 / §6.7.4 — Real-world compiler bug regressions:
 *   - Global struct/array designated + positional initializer overrides
 *   - Static address-constant initializers with positive & negative offsets,
 *     offsetof, nested member pointers, and string literal + offset
 *   - static inline function sharing a static local variable across callers
 *   - Large BSS array adjacent to initialized globals
 */
#include <stddef.h>
#include <stdint.h>
#include "c17_test.h"

struct inner {
    int x;
    int y;
};

struct record {
    int id;
    struct {
        unsigned a : 4;
        unsigned b : 4;
    };
    union {
        int i;
        short s;
    } u;
    struct inner pts[2];
};

/* Mixed designated + positional + overridden initializers (C17 §6.7.9 p19) */
static struct record g_recs[3] = {
    [2] = { .id = 200, .a = 9, .b = 6, .u = { .i = 0x1234 }, .pts = { { 1, 2 }, { 3, 4 } } },
    [0] = { .id = 10, .id = 11, .pts[1].y = 77, .u.s = 55 },
    { 99, { 3, 12 }, { .i = -42 }, { { 5, 6 }, { 7, 8 } } } /* positional -> index [1]! */
};

/* Large zero-initialized BSS buffer between initialized globals */
static unsigned char g_bss_buf[16384];
static int g_after_bss = 0x13572468;

/* Address constant relocations with positive/negative offsets & string offset */
static int g_nums[6] = { 10, 20, 30, 40, 50, 60 };
static int *g_p_mid = &g_nums[4] - 2; /* &g_nums[2] == 30 */
static const char *g_str_off = "abcdef" + 2; /* "cdef" */
static char *g_byte_off = (char *)&g_recs[2] + offsetof(struct record, pts);
static int *g_nested_ptr = &g_recs[1].pts[1].x;

static inline int shared_inline_counter(int delta) {
    static int counter = 10;
    counter += delta;
    return counter;
}

static int caller_one(int d) {
    return shared_inline_counter(d);
}

static int caller_two(int d) {
    return shared_inline_counter(d);
}

int main(void) {
    C17_BEGIN("c67_bug_static_init_reloc",
              "Static designated init overrides, offset relocs, static inline local");

    /* Check overridden & positional global struct array */
    C17_CHECK_EQ(g_recs[0].id, 11);
    C17_CHECK_EQ(g_recs[0].a, 0);
    C17_CHECK_EQ(g_recs[0].b, 0);
    C17_CHECK_EQ(g_recs[0].u.s, 55);
    C17_CHECK_EQ(g_recs[0].pts[0].x, 0);
    C17_CHECK_EQ(g_recs[0].pts[1].y, 77);

    C17_CHECK_EQ(g_recs[1].id, 99);
    C17_CHECK_EQ(g_recs[1].a, 3);
    C17_CHECK_EQ(g_recs[1].b, 12);
    C17_CHECK_EQ(g_recs[1].u.i, -42);
    C17_CHECK_EQ(g_recs[1].pts[0].x, 5);
    C17_CHECK_EQ(g_recs[1].pts[1].y, 8);

    C17_CHECK_EQ(g_recs[2].id, 200);
    C17_CHECK_EQ(g_recs[2].a, 9);
    C17_CHECK_EQ(g_recs[2].b, 6);
    C17_CHECK_EQ(g_recs[2].u.i, 0x1234);
    C17_CHECK_EQ(g_recs[2].pts[1].y, 4);

    /* Address constant relocations */
    C17_CHECK_EQ(*g_p_mid, 30);
    C17_CHECK_STR(g_str_off, "cdef");
    C17_CHECK(g_byte_off == (char *)&g_recs[2].pts);
    C17_CHECK_EQ(*g_nested_ptr, 7);

    /* BSS zero-init & neighbor integrity */
    C17_CHECK_EQ(g_bss_buf[0], 0);
    C17_CHECK_EQ(g_bss_buf[8192], 0);
    C17_CHECK_EQ(g_bss_buf[16383], 0);
    C17_CHECK_EQ(g_after_bss, 0x13572468);

    /* static inline function with static local shares state across callers */
    C17_CHECK_EQ(caller_one(c17_opaque_int(5)), 15);
    C17_CHECK_EQ(caller_two(c17_opaque_int(7)), 22);
    C17_CHECK_EQ(shared_inline_counter(c17_opaque_int(3)), 25);

    C17_END();
}
