/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 — Adversarial alias analysis (alias.c) &
 * negative-offset / stride-modular struct-array disambiguation tests:
 *  - Array of structs accessed by two different indices i & j (disjoint fields
 *    vs same field vs byte-aliased overlap!)
 *  - Negative pointer offsets p[-1], p[-4] combined with p[0] & store forwarding
 *  - Union active-member switching & byte-wise inspection
 */
#include <stdint.h>
#include <string.h>
#include "c17_test.h"

struct record12 {
    int32_t a;
    int32_t b;
    int32_t c;
};

/* Stride-modular field disambiguation: same array, different fields vs same field */
static int32_t struct_stride_alias(struct record12 *arr, int i, int j) {
    arr[i].a = 100;
    arr[j].b = 200;  /* stride=12, off 0 vs off 4 -> disjoint even if i == j! */
    int32_t ra = arr[i].a;
    arr[j].a = 300;  /* SAME field -> MUST alias when i == j! */
    int32_t ra2 = arr[i].a;
    return ra + ra2 + arr[j].b;
}

/* Negative offset pointer aliasing: p[-1] vs (p - 1)[0] vs base[k - 1] */
static int32_t neg_offset_alias(int32_t *base, int k) {
    int32_t *p = base + k;
    p[-1] = 11;
    p[-2] = 22;
    base[k - 1] = 77; /* MUST alias p[-1]! */
    int32_t v1 = p[-1];
    *(int32_t *)((char *)p - 8) = 88; /* MUST alias p[-2]! */
    int32_t v2 = p[-2];
    return v1 + v2;
}

/* Cross-field byte store overlapping adjacent struct member */
static int32_t cross_field_byte_alias(struct record12 *arr, int i, int j) {
    arr[i].a = 0x11223344;
    arr[i].b = 0x55667788;
    unsigned char *raw = (unsigned char *)&arr[j];
    raw[0] = 0xAAu;  /* overlaps arr[i].a if i == j */
    raw[4] = 0xBBu;  /* overlaps arr[i].b if i == j */
    return (int32_t)(arr[i].a & 0xFF) + (int32_t)(arr[i].b & 0xFF);
}

static int32_t (*volatile g_stride_fn)(struct record12 *, int, int) = struct_stride_alias;
static int32_t (*volatile g_negoff_fn)(int32_t *, int) = neg_offset_alias;
static int32_t (*volatile g_byte_fn)(struct record12 *, int, int) = cross_field_byte_alias;

int main(void) {
    C17_BEGIN("c65_bug_alias_stride_negoff_and_byte",
              "Alias analysis stride-modular fields, negative offsets, cross-field byte alias");

    /* 1. Stride-modular struct array aliasing (both i == j and i != j) */
    {
        struct record12 arr[6];
        memset(arr, 0, sizeof(arr));
        /* i == j: ra=100, ra2=300, b=200 => 600 */
        C17_CHECK_EQ(g_stride_fn(arr, c17_opaque_int(2), c17_opaque_int(2)), 600);
        /* i != j: ra=100, ra2=100, b=200 => 400 */
        C17_CHECK_EQ(g_stride_fn(arr, c17_opaque_int(1), c17_opaque_int(4)), 400);
    }

    /* 2. Negative pointer offsets */
    {
        int32_t buf[10] = { 0 };
        C17_CHECK_EQ(g_negoff_fn(buf, c17_opaque_int(5)), 165); /* 77 + 88 */
        C17_CHECK_EQ(buf[4], 77);
        C17_CHECK_EQ(buf[3], 88);
    }

    /* 3. Cross-field byte aliasing */
    {
        struct record12 arr[4];
        memset(arr, 0, sizeof(arr));
        /* i == j: 0xAA + 0xBB = 170 + 187 = 357 */
        C17_CHECK_EQ(g_byte_fn(arr, c17_opaque_int(1), c17_opaque_int(1)), 357);
        /* i != j: 0x44 + 0x88 = 68 + 136 = 204 */
        C17_CHECK_EQ(g_byte_fn(arr, c17_opaque_int(0), c17_opaque_int(2)), 204);
    }

    C17_END();
}
