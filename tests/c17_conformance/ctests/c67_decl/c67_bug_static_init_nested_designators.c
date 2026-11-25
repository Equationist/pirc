/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9, §6.7.2.1, §6.5.3.4 — Adversarial static &
 * automatic initializers (exact-fit non-NUL strings, nested sub-object
 * designator overrides, union non-first member & override, static pointer
 * arithmetic relocations), FAM offsetof/sizeof, and sizeof VLA side-effects.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "c17_test.h"

struct tag_box {
    char tag[4];
    int val;
};

struct inner {
    int a, b, c;
};

struct outer {
    struct inner in;
    int tail;
};

union num_u {
    int i;
    double d;
    struct { uint16_t lo, hi; } halves;
};

struct fam_pkt {
    uint32_t len;
    uint8_t pad;
    uint32_t data[];
};

/* Static / file-scope exact-fit string (no NUL terminator) inside struct */
static struct tag_box g_tag = { "ABCD", 42 };

/* Static nested sub-object designated override */
static struct outer g_out = { .in = { 1, 2, 3 }, .in.b = 99, .tail = 7 };

/* Static union non-first member & member override */
static union num_u g_ud = { .d = 3.5 };
static union num_u g_uh = { .i = 0x12345678, .halves = { 0x1111u, 0x2222u } };

/* Static pointer arithmetic & nested member address relocations */
static int g_arr[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
static int *g_p_sub = &g_arr[5] - 2; /* points to g_arr[3] == 40 */
static struct outer g_outs[3] = {
    { { 1, 2, 3 }, 10 },
    { { 4, 5, 6 }, 20 },
    { { 7, 8, 9 }, 30 }
};
static int *g_p_nested = &g_outs[2].in.b; /* points to 8 */
static const char *g_str_off = "hello_world" + 6; /* "world" */

static int g_se = 0;
static int bump(int v) {
    g_se++;
    return v;
}

int main(void) {
    C17_BEGIN("c67_bug_static_init_nested_designators",
              "Static/auto exact-fit strings, nested designators, relocs, FAM, sizeof VLA");

    /* 1. Static exact-fit non-NUL string inside struct */
    C17_CHECK_EQ(g_tag.tag[0], 'A');
    C17_CHECK_EQ(g_tag.tag[3], 'D');
    C17_CHECK_EQ(g_tag.val, 42);

    /* Auto exact-fit non-NUL string inside struct */
    {
        struct tag_box lt = { "WXYZ", 99 };
        C17_CHECK_EQ(lt.tag[0], 'W');
        C17_CHECK_EQ(lt.tag[3], 'Z');
        C17_CHECK_EQ(lt.val, 99);
    }

    /* 2. Static & auto nested sub-object designated override */
    C17_CHECK_EQ(g_out.in.a, 1);
    C17_CHECK_EQ(g_out.in.b, 99);
    C17_CHECK_EQ(g_out.in.c, 3);
    C17_CHECK_EQ(g_out.tail, 7);
    {
        int dyn = c17_opaque_int(55);
        struct outer lo = { .in = { 10, 20, 30 }, .in.b = dyn, .tail = 70 };
        C17_CHECK_EQ(lo.in.a, 10);
        C17_CHECK_EQ(lo.in.b, 55);
        C17_CHECK_EQ(lo.in.c, 30);
        C17_CHECK_EQ(lo.tail, 70);
    }

    /* 3. Static union non-first member & override */
    C17_CHECK(g_ud.d == 3.5);
    C17_CHECK_EQ(g_uh.halves.lo, 0x1111u);
    C17_CHECK_EQ(g_uh.halves.hi, 0x2222u);

    /* 4. Static pointer arithmetic relocations */
    C17_CHECK_EQ(*g_p_sub, 40);
    C17_CHECK_EQ(*g_p_nested, 8);
    C17_CHECK_EQ(g_str_off[0], 'w');
    C17_CHECK_EQ(g_str_off[4], 'd');

    /* 5. Flexible array member layout & dynamic allocation */
    {
        C17_CHECK(sizeof(struct fam_pkt) >= offsetof(struct fam_pkt, data));
        C17_CHECK_EQ(offsetof(struct fam_pkt, data), 8u);
        struct fam_pkt *pkt = (struct fam_pkt *)malloc(
            offsetof(struct fam_pkt, data) + 4u * sizeof(uint32_t));
        C17_CHECK(pkt != NULL);
        if (pkt != NULL) {
            pkt->len = 4u;
            pkt->pad = 0xAAu;
            for (uint32_t i = 0; i < 4u; i++) pkt->data[i] = (i + 1u) * 100u;
            C17_CHECK_EQ(pkt->data[0] + pkt->data[3], 500u);
            free(pkt);
        }
    }

    /* 6. sizeof: non-VLA operand MUST NOT evaluate side effects;
     *    VLA type-name operand MUST evaluate bound expression! */
    {
        int fixed[5];
        g_se = 0;
        size_t sz1 = sizeof(fixed[bump(2)]);
        C17_CHECK_EQ(sz1, sizeof(int));
        C17_CHECK_EQ(g_se, 0);

        int n = c17_opaque_int(6);
        int vla[n];
        size_t sz2 = sizeof(vla);
        C17_CHECK_EQ(sz2, (size_t)n * sizeof(int));

        /* sizeof(int[bump(7)]) MUST evaluate bump(7) because type is VLA! */
        size_t sz3 = sizeof(int[bump(7)]);
        C17_CHECK_EQ(sz3, 7u * sizeof(int));
        C17_CHECK_EQ(g_se, 1);

        /* sizeof(int (*)[bump(9)]): pointer to VLA is variably modified ->
         * C17 §6.5.3.4p2: operand IS evaluated if type is VLA; pointer itself
         * is NOT a VLA (just VM), so test direct VLA 2D type instead! */
        size_t sz4 = sizeof(int[bump(3)][4]);
        C17_CHECK_EQ(sz4, 12u * sizeof(int));
        C17_CHECK_EQ(g_se, 2);
    }

    C17_END();
}
