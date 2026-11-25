/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 — Real-world bitfield & union SROA/GVN/DSE bugs:
 *   - Union overlaying bitfield struct and uint32_t word (SROA/GVN/DSE partial-word
 *     store-forwarding bug from GCC/LLVM)
 *   - _Bool : 1 bitfield normalization (assigning 2, 256 -> must store 1!)
 *   - Bitfields at non-zero byte offset inside a larger struct (TCC/chibicc/QBE)
 *   - Global & local static bitfield initializers with negative signed bitfields
 */
#include <stdbool.h>
#include <stdint.h>
#include "c17_test.h"

union reg_union {
    struct {
        unsigned int lo : 16;
        unsigned int hi : 16;
    } bits;
    uint32_t word;
};

struct offset_bf {
    uint8_t prefix;
    _Bool flag : 1;
    signed int sval : 5;    /* -16..15 */
    unsigned int uval : 10; /* 0..1023 */
    uint16_t suffix;
};

static struct offset_bf g_bf_init = {
    .prefix = 0xAA,
    .flag = 1,
    .sval = -13,
    .uval = 777,
    .suffix = 0xBEEF
};

/* Classic GCC/LLVM SROA/GVN/DSE union-bitfield store-forwarding pattern */
static uint32_t reg_union_write_both(unsigned lo, unsigned hi) {
    union reg_union u;
    u.word = 0U;
    u.bits.lo = lo;
    u.bits.hi = hi;
    return u.word;
}

static uint32_t reg_union_partial_overwrite(uint32_t init, unsigned new_lo) {
    union reg_union u;
    u.word = init;
    u.bits.lo = new_lo;
    return u.word;
}

int main(void) {
    C17_BEGIN("c67_bug_bitfield_union_sroa",
              "Union bitfield/word SROA/GVN/DSE, _Bool:1 bitfield, non-zero offset bf");

    /* Union bitfield <-> uint32_t word (endian-safe verification) */
    {
        uint32_t w1 = reg_union_write_both(
            (unsigned)c17_opaque_int(0x1234),
            (unsigned)c17_opaque_int(0x5678));
        union reg_union check;
        check.word = w1;
        C17_CHECK_UEQ(check.bits.lo, 0x1234U);
        C17_CHECK_UEQ(check.bits.hi, 0x5678U);
        C17_CHECK(w1 != 0U);

        uint32_t w2 = reg_union_partial_overwrite(w1, (unsigned)c17_opaque_int(0xABCD));
        check.word = w2;
        C17_CHECK_UEQ(check.bits.lo, 0xABCDU);
        C17_CHECK_UEQ(check.bits.hi, 0x5678U);
    }

    /* Static global bitfield initializer */
    C17_CHECK_UEQ(g_bf_init.prefix, 0xAAU);
    C17_CHECK_EQ((int)g_bf_init.flag, 1);
    C17_CHECK_EQ(g_bf_init.sval, -13);
    C17_CHECK_EQ(g_bf_init.uval, 777);
    C17_CHECK_UEQ(g_bf_init.suffix, 0xBEEFU);

    /* _Bool : 1 bitfield normalization (256 -> 1, NOT truncated to 0!) */
    {
        struct offset_bf s = { 0x55, false, -7, 333, 0x1234 };
        s.flag = (_Bool)c17_opaque_int(256);
        C17_CHECK_EQ((int)s.flag, 1);
        C17_CHECK_UEQ(s.prefix, 0x55U);
        C17_CHECK_EQ(s.sval, -7);
        C17_CHECK_EQ(s.uval, 333);
        C17_CHECK_UEQ(s.suffix, 0x1234U);

        s.flag = (_Bool)c17_opaque_int(2);
        C17_CHECK_EQ((int)s.flag, 1);
        s.flag = (_Bool)c17_opaque_int(0);
        C17_CHECK_EQ((int)s.flag, 0);

        /* Mutating sval and uval must preserve prefix, flag, and suffix */
        s.sval = -16;
        s.uval = 1023;
        C17_CHECK_UEQ(s.prefix, 0x55U);
        C17_CHECK_EQ((int)s.flag, 0);
        C17_CHECK_EQ(s.sval, -16);
        C17_CHECK_EQ(s.uval, 1023);
        C17_CHECK_UEQ(s.suffix, 0x1234U);
    }

    C17_END();
}
