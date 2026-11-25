/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 — Extreme bitfield bugs from GCC, LLVM, TCC,
 * chibicc, and QBE:
 *   - 1-bit signed bitfield (`signed int s1 : 1` -> values 0 and -1, NEVER +1!)
 *   - Full 32-bit bitfields (`unsigned int u32 : 32`, `signed int s32 : 32`)
 *     catching compiler shift-by-32 bugs (`1U << 32`)
 *   - Multi-word odd-width bitfields (5 x 13-bit = 65 bits crossing 32/64B edges)
 *   - Bitfield compound shift/add/xor assignments and arithmetic promotions
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

struct bf_one_bit {
    signed int s1 : 1;
    unsigned int u1 : 1;
    signed int s2 : 2;   /* -2..1 */
    unsigned int u2 : 2; /* 0..3 */
};

struct bf_full32 {
    unsigned int u32 : 32;
    signed int s32 : 32;
};

struct bf_multi13 {
    unsigned int a : 13;
    signed int   b : 13;
    unsigned int c : 13;
    signed int   d : 13;
    unsigned int e : 13;
};

int main(void) {
    C17_BEGIN("c67_bug_bitfield_extreme",
              "1-bit signed bf (-1/0), full 32-bit bf, 65-bit multi-word odd bf");

    /* 1-bit signed bitfield: assigning 1 -> -1, assigning 0 -> 0! */
    {
        struct bf_one_bit b = { 0, 0, 0, 0 };
        b.s1 = c17_opaque_int(1);
        b.u1 = (unsigned)c17_opaque_int(1);
        b.s2 = c17_opaque_int(-2);
        b.u2 = (unsigned)c17_opaque_int(3);
        C17_CHECK_EQ(b.s1, -1);
        C17_CHECK_EQ(b.u1, 1);
        C17_CHECK_EQ(b.s2, -2);
        C17_CHECK_EQ(b.u2, 3);
        C17_CHECK_EQ(b.s1 + 1, 0);
        C17_CHECK_EQ(~b.s1, 0);
        b.s1 = c17_opaque_int(0);
        C17_CHECK_EQ(b.s1, 0);
        C17_CHECK_EQ(b.u1, 1);
        C17_CHECK_EQ(b.s2, -2);
        C17_CHECK_EQ(b.u2, 3);
    }

    /* Full 32-bit bitfields (tests compiler against (1U << 32) UB!) */
    {
        struct bf_full32 f = { 0U, 0 };
        f.u32 = (unsigned)c17_opaque_ull(0xDEADBEEFULL);
        f.s32 = c17_opaque_int(INT_MIN);
        C17_CHECK_UEQ(f.u32, 0xDEADBEEFU);
        C17_CHECK_EQ(f.s32, INT_MIN);
        f.u32 += 1U;
        C17_CHECK_UEQ(f.u32, 0xDEADBEF0U);
        C17_CHECK_EQ(f.s32, INT_MIN);
        f.u32 = 0xFFFFFFFFU;
        f.u32++;
        C17_CHECK_UEQ(f.u32, 0U);
        C17_CHECK_EQ(f.s32, INT_MIN);
    }

    /* 5 x 13-bit = 65 bits spanning multiple 32-bit storage units */
    {
        struct bf_multi13 m = { 0, 0, 0, 0, 0 };
        m.a = (unsigned)c17_opaque_int(8191);
        m.b = c17_opaque_int(-4096);
        m.c = (unsigned)c17_opaque_int(5555);
        m.d = c17_opaque_int(-1234);
        m.e = (unsigned)c17_opaque_int(7777);

        C17_CHECK_EQ(m.a, 8191);
        C17_CHECK_EQ(m.b, -4096);
        C17_CHECK_EQ(m.c, 5555);
        C17_CHECK_EQ(m.d, -1234);
        C17_CHECK_EQ(m.e, 7777);

        /* Compound shift/arithmetic on middle bitfield must not clobber neighbors */
        m.c >>= 2;
        m.e ^= 0x1555U;
        C17_CHECK_EQ(m.a, 8191);
        C17_CHECK_EQ(m.b, -4096);
        C17_CHECK_EQ(m.c, 5555 >> 2);
        C17_CHECK_EQ(m.d, -1234);
        C17_CHECK_EQ(m.e, (7777U ^ 0x1555U) & 0x1FFFU);
    }

    C17_END();
}
