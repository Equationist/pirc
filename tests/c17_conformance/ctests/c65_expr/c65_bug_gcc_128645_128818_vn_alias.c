/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5, §6.7.3 — Real-world 2025/2026 GCC & LLVM
 * optimizer miscompilation regressions:
 *  - GCC PR128645: Partial PRE double/long load aliasing across conditional store
 *  - GCC PR128818 / PR128688: SCCVN same-value store skipping & restrict inlining
 *  - LLVM #186922: Auto-vectorized i32 copy loops (odd lengths, offset slices)
 *  - LLVM #203703: Byte-alias (unsigned char *) update inside loop
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "c17_test.h"

/* --- 1. GCC PR128645: Partial PRE double/long load aliasing on malloc'd buf --- */
static long pr128645_fn(char *b, int o, int m, int k) {
    *(long *)b = 1;
    if (m) {
        *(double *)(b + o) = 5.0;
    } else {
        *(long *)b = 2;
    }
    if (k) {
        return (long)*(double *)b;
    }
    return *(long *)b;
}

static long (*volatile g_pr128645)(char *, int, int, int) = pr128645_fn;

/* --- 2. GCC PR128818 / PR128688: SCCVN same-value store & restrict helper --- */
static inline uint64_t pr128818_helper(uint64_t *restrict p, uint64_t *y, int k, int c) {
    *y = 2u;
    p[k] = 0u;
    uint64_t r = 0u;
    if (c) {
        r = *p;
    }
    return r;
}

static uint64_t pr128818_disjoint(uint64_t *x, uint64_t *y, int k, int c) {
    memset(x, 0, 16);
    uint64_t r = pr128818_helper(x, y, k, c);
    return r + *x;
}

/* Non-restrict version where x and y DO alias (x == y, k == 0 or k == 1)! */
static uint64_t pr128818_aliased(uint64_t *x, uint64_t *y, int k, int c) {
    x[0] = 0u;
    x[1] = 0u;
    *y = 2u;       /* If y == x, x[0] is now 2! */
    x[k] = 0u;     /* Same-value as original x[k]=0, MUST NOT be skipped if k==0! */
    uint64_t r = 0u;
    if (c) {
        r = *x;
    }
    return r + *x;
}

static uint64_t (*volatile g_pr128818_disj)(uint64_t *, uint64_t *, int, int) = pr128818_disjoint;
static uint64_t (*volatile g_pr128818_alias)(uint64_t *, uint64_t *, int, int) = pr128818_aliased;

/* --- 3. LLVM #186922: Vectorized i32 copy loops with odd lengths & strides --- */
static void copy_i32_loop(int32_t *dst, const int32_t *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

static void copy_i32_transform(int32_t *dst, const int32_t *src, int n, int32_t bias) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i] + bias;
    }
}

static void (*volatile g_copy_i32)(int32_t *, const int32_t *, int) = copy_i32_loop;
static void (*volatile g_copy_xform)(int32_t *, const int32_t *, int, int32_t) = copy_i32_transform;

/* --- 4. LLVM #203703: Byte-alias mutation inside loop --- */
struct pair32 {
    uint32_t lo;
    uint32_t hi;
};

static uint32_t byte_alias_loop(struct pair32 *p, int n) {
    unsigned char *raw = (unsigned char *)p;
    uint32_t acc = 0u;
    for (int i = 0; i < n; i++) {
        p->lo += 1u;
        raw[0] = (unsigned char)(raw[0] ^ 0x5Au);
        acc += p->lo;
        raw[4] = (unsigned char)(raw[4] + 3u);
        acc += p->hi;
    }
    return acc;
}

static uint32_t (*volatile g_byte_alias)(struct pair32 *, int) = byte_alias_loop;

int main(void) {
    C17_BEGIN("c65_bug_gcc_128645_128818_vn_alias",
              "GCC PR128645/PR128818 & LLVM #186922/#203703 alias/VN/vec bugs");

    /* 1. GCC PR128645 */
    {
        char *buf = (char *)malloc(16);
        C17_CHECK(buf != NULL);
        if (buf != NULL) {
            C17_CHECK_EQ(g_pr128645(buf, c17_opaque_int(0), c17_opaque_int(1), c17_opaque_int(1)), 5L);
            C17_CHECK_EQ(g_pr128645(buf, c17_opaque_int(0), c17_opaque_int(0), c17_opaque_int(0)), 2L);
            free(buf);
        }
    }

    /* 2. GCC PR128818 / PR128688 */
    {
        uint64_t a[2] = { 99u, 88u };
        uint64_t b = 77u;
        C17_CHECK_EQ(g_pr128818_disj(a, &b, c17_opaque_int(0), c17_opaque_int(1)), 0u);
        C17_CHECK_EQ(b, 2u);

        /* Aliased: x == y, k == 0 => x[0] becomes 2 then overwritten with 0 => 0 + 0 = 0 */
        uint64_t c[2] = { 11u, 22u };
        C17_CHECK_EQ(g_pr128818_alias(c, c, c17_opaque_int(0), c17_opaque_int(1)), 0u);
        C17_CHECK_EQ(c[0], 0u);

        /* Aliased: x == y, k == 1 => x[0] becomes 2, x[1] becomes 0 => 2 + 2 = 4 */
        C17_CHECK_EQ(g_pr128818_alias(c, c, c17_opaque_int(1), c17_opaque_int(1)), 4u);
        C17_CHECK_EQ(c[0], 2u);
        C17_CHECK_EQ(c[1], 0u);
    }

    /* 3. LLVM #186922: i32 copy loops across odd/even/small/large sizes & overlapping buf */
    {
        int32_t src[37], dst[37];
        for (int i = 0; i < 37; i++) {
            src[i] = (i * 17) - 100;
            dst[i] = 0x7777;
        }
        int lengths[6] = { 0, 1, 3, 7, 17, 37 };
        for (int li = 0; li < 6; li++) {
            int n = lengths[li];
            for (int i = 0; i < 37; i++) dst[i] = -999;
            g_copy_i32(dst, src, n);
            for (int i = 0; i < n; i++) C17_CHECK_EQ(dst[i], src[i]);
            for (int i = n; i < 37; i++) C17_CHECK_EQ(dst[i], -999);
        }

        /* Overlapping forward shift dst = buf + 1, src = buf (propagates buf[0]!) */
        int32_t ov[21];
        for (int i = 0; i < 21; i++) ov[i] = i + 10;
        g_copy_i32(ov + 1, ov, c17_opaque_int(20));
        for (int i = 0; i < 21; i++) C17_CHECK_EQ(ov[i], 10);

        /* Overlapping with bias: ov[i+1] = ov[i] + 3 => arithmetic progression */
        ov[0] = 5;
        for (int i = 1; i < 21; i++) ov[i] = 0;
        g_copy_xform(ov + 1, ov, c17_opaque_int(20), c17_opaque_int(3));
        for (int i = 0; i < 21; i++) C17_CHECK_EQ(ov[i], 5 + i * 3);
    }

    /* 4. LLVM #203703: Byte-alias mutation inside loop */
    {
        struct pair32 p = { 0u, 0u };
        uint32_t got = g_byte_alias(&p, c17_opaque_int(5));
        /* Reference trace (little-endian low byte XOR 0x5A):
         * i=0: lo=1 -> ^0x5A = 0x5B (91); hi byte0=3 -> hi=3; acc=94
         * i=1: lo=0x5C -> ^0x5A = 0x06 (6); hi=6; acc=106
         * i=2: lo=7 -> ^0x5A = 0x5D (93); hi=9; acc=208
         * i=3: lo=0x5E -> ^0x5A = 0x04 (4); hi=12; acc=224
         * i=4: lo=5 -> ^0x5A = 0x5F (95); hi=15; acc=334 */
        C17_CHECK_EQ(p.lo, 95u);
        C17_CHECK_EQ(p.hi, 15u);
        C17_CHECK_EQ(got, 334u);
    }

    C17_END();
}
