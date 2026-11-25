/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 — Real-world compiler bug regressions:
 *   - Csmith / GCC / LLVM bitfield + SROA + GVN aliasing bugs:
 *     packed signed/unsigned bitfields mutated via pointer, passed/returned
 *     by value, compound assignment & pre/post increment wraparound, ternary
 *     promotion, and zero-width (:0) alignment boundaries.
 */
#include <stdint.h>
#include "c17_test.h"

struct bf_pack {
    signed int a : 3;     /* -4..3 */
    unsigned int b : 5;   /* 0..31 */
    signed int c : 7;     /* -64..63 */
    unsigned int d : 17;  /* 0..131071 */
};

struct bf_zero_align {
    unsigned int x : 5;
    unsigned int : 0;
    unsigned int y : 5;
};

static void mutate_bf(struct bf_pack *p, int da, unsigned db, int dc) {
    p->a = (signed int)(p->a + da);
    p->b = (unsigned int)(p->b + db);
    p->c = (signed int)(p->c + dc);
}

static struct bf_pack make_and_tweak(int a, unsigned b, int c, unsigned d) {
    struct bf_pack s = { 0, 0, 0, 0 };
    s.a = a;
    s.b = b;
    s.c = c;
    s.d = d;
    return s;
}

static int bf_ternary(const struct bf_pack *p, int sel) {
    /* Both signed:3 and unsigned:5 promote to int (since int represents 0..31) */
    return sel ? p->a : p->b;
}

int main(void) {
    C17_BEGIN("c65_bug_csmith_bitfield_sroa",
              "Csmith/GCC/LLVM bitfield SROA, aliasing, wraparound & :0 boundary");

    /* Basic packing, sign extension, and zero corruption of neighbors */
    {
        struct bf_pack s = make_and_tweak(
            c17_opaque_int(-3),
            (unsigned)c17_opaque_int(29),
            c17_opaque_int(-50),
            (unsigned)c17_opaque_int(100000));
        C17_CHECK_EQ(s.a, -3);
        C17_CHECK_EQ(s.b, 29);
        C17_CHECK_EQ(s.c, -50);
        C17_CHECK_EQ(s.d, 100000);

        /* Mutating s.a and s.b must NOT clobber s.c or s.d */
        mutate_bf(&s, c17_opaque_int(5), (unsigned)c17_opaque_int(5), c17_opaque_int(20));
        C17_CHECK_EQ(s.a, 2);
        C17_CHECK_EQ(s.b, 2); /* (29 + 5) & 31 == 2 */
        C17_CHECK_EQ(s.c, -30);
        C17_CHECK_EQ(s.d, 100000);
    }

    /* Unsigned bitfield increment/decrement wraparound & return value */
    {
        struct bf_pack s = { 0, 31, 0, 0 };
        unsigned prev = s.b++;
        C17_CHECK_EQ(prev, 31);
        C17_CHECK_EQ(s.b, 0);

        unsigned next = --s.b;
        C17_CHECK_EQ(next, 31);
        C17_CHECK_EQ(s.b, 31);

        s.b ^= 0x15U;
        C17_CHECK_EQ(s.b, 0x0AU);
        C17_CHECK_EQ(s.a, 0);
        C17_CHECK_EQ(s.c, 0);
        C17_CHECK_EQ(s.d, 0);
    }

    /* Ternary promotion on signed:3 vs unsigned:5 -> must yield signed -3! */
    {
        struct bf_pack s = { -3, 12, -1, 7 };
        C17_CHECK_EQ(bf_ternary(&s, c17_opaque_int(1)), -3);
        C17_CHECK_EQ(bf_ternary(&s, c17_opaque_int(0)), 12);
    }

    /* Struct copy preserves all bitfields without leaking bits across fields */
    {
        struct bf_pack s1 = { -4, 31, -64, 131071 };
        struct bf_pack s2 = s1;
        s1.a = 1;
        s1.b = 0;
        C17_CHECK_EQ(s2.a, -4);
        C17_CHECK_EQ(s2.b, 31);
        C17_CHECK_EQ(s2.c, -64);
        C17_CHECK_EQ(s2.d, 131071);
    }

    /* Zero-width bitfield forces separate storage unit */
    {
        struct bf_zero_align z = { 0, 0 };
        z.x = (unsigned)c17_opaque_int(27);
        z.y = (unsigned)c17_opaque_int(19);
        C17_CHECK(sizeof(struct bf_zero_align) >= 2 * sizeof(unsigned int));
        C17_CHECK_EQ(z.x, 27);
        C17_CHECK_EQ(z.y, 19);
    }

    C17_END();
}
