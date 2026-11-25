/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1, §6.3.1.1 — Bitfields inside unions,
 * volatile bitfield RMW preservation, _Bool bitfield increment/coercion
 * (TinyCC/chibicc bug), and signed/unsigned bitfield integer promotions.
 */
#include <stdint.h>
#include "c17_test.h"

struct bf_union_holder {
    int tag;
    union {
        uint32_t raw;
        unsigned char bytes[4];
        struct {
            unsigned a : 5;
            unsigned b : 7;
            unsigned c : 11;
            unsigned d : 9;
        } bits;
    } u;
};

struct bool_and_signed_bf {
    _Bool flag : 1;
    signed int s1 : 1;
    signed int s5 : 5;
    unsigned int u5 : 5;
    unsigned int u19 : 19;
};

static uint32_t mutate_union_bf(struct bf_union_holder *h,
                                unsigned a, unsigned b, unsigned c, unsigned d) {
    h->u.raw = 0xA5A5A5A5u;
    h->u.bits.b = b;
    h->u.bits.d = d;
    h->u.bits.a = a;
    h->u.bits.c = c;
    return h->u.raw;
}

static uint32_t volatile_bf_rmw(volatile struct bf_union_holder *vh) {
    vh->u.bits.a = 0x15u;
    vh->u.bits.c = 0x3AAu;
    uint32_t r1 = vh->u.bits.a + vh->u.bits.c;
    vh->u.bits.a = 0x0Au;
    uint32_t r2 = vh->u.bits.a + vh->u.bits.c;
    return r1 * 1000u + r2;
}

static uint32_t (*volatile g_mut_bf)(struct bf_union_holder *,
                                     unsigned, unsigned, unsigned, unsigned) = mutate_union_bf;
static uint32_t (*volatile g_vol_bf)(volatile struct bf_union_holder *) = volatile_bf_rmw;

int main(void) {
    C17_BEGIN("c65_bug_union_bitfield_volatile_rmw",
              "Union-overlaid bitfields, volatile RMW, _Bool bf++, & promotions");

    /* 1. Bitfields inside union inside struct */
    {
        struct bf_union_holder h = { 1, { 0u } };
        uint32_t raw = g_mut_bf(&h, c17_opaque_int(0x1Bu), c17_opaque_int(0x55u),
                                c17_opaque_int(0x6C3u), c17_opaque_int(0x1F2u));
        C17_CHECK_EQ(h.u.bits.a, 0x1Bu);
        C17_CHECK_EQ(h.u.bits.b, 0x55u);
        C17_CHECK_EQ(h.u.bits.c, 0x6C3u);
        C17_CHECK_EQ(h.u.bits.d, 0x1F2u);
        /* Verify untouched-neighbor masking when overwriting a single middle field */
        h.u.bits.b = 0x2Au;
        C17_CHECK_EQ(h.u.bits.a, 0x1Bu);
        C17_CHECK_EQ(h.u.bits.b, 0x2Au);
        C17_CHECK_EQ(h.u.bits.c, 0x6C3u);
        C17_CHECK_EQ(h.u.bits.d, 0x1F2u);
        /* Verify byte overlay reflects the exact bit pattern */
        uint32_t recon = (uint32_t)h.u.bytes[0]
                       | ((uint32_t)h.u.bytes[1] << 8)
                       | ((uint32_t)h.u.bytes[2] << 16)
                       | ((uint32_t)h.u.bytes[3] << 24);
        C17_CHECK_EQ(recon, h.u.raw);
        C17_CHECK(raw != h.u.raw);
    }

    /* 2. Volatile struct with bitfields inside union */
    {
        volatile struct bf_union_holder vh = { 2, { 0u } };
        uint32_t res = g_vol_bf(&vh);
        /* r1 = 0x15 + 0x3AA = 21 + 938 = 959; r2 = 10 + 938 = 948 */
        C17_CHECK_EQ(res, 959948u);
        C17_CHECK_EQ(vh.u.bits.a, 0x0Au);
        C17_CHECK_EQ(vh.u.bits.c, 0x3AAu);
    }

    /* 3. _Bool bitfield coercion & increment (TinyCC / chibicc classic bug) */
    {
        struct bool_and_signed_bf s = { 0, 0, 0, 0, 0 };
        s.flag = c17_opaque_int(256);   /* must coerce to 1, NOT truncate to 0! */
        C17_CHECK_EQ((int)s.flag, 1);
        s.flag = c17_opaque_dbl(0.25);  /* non-zero double -> 1 */
        C17_CHECK_EQ((int)s.flag, 1);
        s.flag = c17_opaque_dbl(0.0);   /* zero double -> 0 */
        C17_CHECK_EQ((int)s.flag, 0);

        /* _Bool++ on 0 -> yields 0, becomes 1; _Bool++ on 1 -> yields 1, stays 1! */
        int p0 = s.flag++;
        C17_CHECK_EQ(p0, 0);
        C17_CHECK_EQ((int)s.flag, 1);
        int p1 = s.flag++;
        C17_CHECK_EQ(p1, 1);
        C17_CHECK_EQ((int)s.flag, 1);
        int p2 = ++s.flag;
        C17_CHECK_EQ(p2, 1);
        C17_CHECK_EQ((int)s.flag, 1);
        int p3 = s.flag--;
        C17_CHECK_EQ(p3, 1);
        C17_CHECK_EQ((int)s.flag, 0);
        int p4 = --s.flag; /* (0 - 1) coerced to _Bool -> 1! */
        C17_CHECK_EQ(p4, 1);
        C17_CHECK_EQ((int)s.flag, 1);
    }

    /* 4. Signed & unsigned bitfield promotions (C17 §6.3.1.1 p2) */
    {
        struct bool_and_signed_bf s = { 0, -1, -13, 29u, 0x54321u };
        C17_CHECK_EQ(s.s1, -1);
        C17_CHECK_EQ(s.s5, -13);
        /* Unsigned 5-bit & 19-bit bitfields fit in `int` -> promote to SIGNED `int`! */
        C17_CHECK(s.u5 - 40 < 0);
        C17_CHECK_EQ(s.u5 - 40, -11);
        C17_CHECK(s.u19 - 0x60000 < 0);
        /* Signed bitfield bitwise complement & arithmetic right-shift */
        C17_CHECK_EQ(~s.s5, 12);
        C17_CHECK_EQ((s.s5 + 0) * 4, -52);
        C17_CHECK_EQ((s.u5 << 2), 116);
    }

    C17_END();
}
