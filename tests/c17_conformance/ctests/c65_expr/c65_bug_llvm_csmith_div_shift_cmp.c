/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.5 / §6.5.8 / §6.5.15 — Real-world compiler bugs:
 *   - Magic-number strength-reduction bugs for signed & unsigned constant
 *     division/modulo (3, 5, 7, 10, 100, -3, -7) at INT_MIN/INT_MAX/UINT_MAX/UINT64_MAX
 *   - Signed vs unsigned comparison folding & chained equality
 *   - Nested ternary select / cmov / csel flag-clobbering patterns
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

static int32_t sdiv3(int32_t x) { return x / 3; }
static int32_t srem3(int32_t x) { return x % 3; }
static int32_t sdiv7(int32_t x) { return x / 7; }
static int32_t sdiv_m7(int32_t x) { return x / -7; }
static int32_t sdiv10(int32_t x) { return x / 10; }
static uint32_t udiv3(uint32_t x) { return x / 3U; }
static uint32_t urem3(uint32_t x) { return x % 3U; }
static uint32_t udiv7(uint32_t x) { return x / 7U; }
static uint32_t udiv10(uint32_t x) { return x / 10U; }

static int64_t sdiv64_3(int64_t x) { return x / 3LL; }
static int64_t sdiv64_m10(int64_t x) { return x / -10LL; }
static uint64_t udiv64_3(uint64_t x) { return x / 3ULL; }
static uint64_t urem64_7(uint64_t x) { return x % 7ULL; }
static uint64_t udiv64_10(uint64_t x) { return x / 10ULL; }

static int nested_select(int a, int b, int c, int d, int e, int f, int g, int h) {
    return (a < b) ? ((c > d) ? (e + 1) : (f + 2))
                   : ((g <= h) ? (e - 3) : (f - 4));
}

int main(void) {
    C17_BEGIN("c65_bug_llvm_csmith_div_shift_cmp",
              "Constant div/rem magic-mul strength reduction, signed/unsigned cmp, nested select");

    /* 32-bit signed constant division/modulo at extreme values */
    {
        int32_t min32 = c17_opaque_int(INT32_MIN);
        int32_t max32 = c17_opaque_int(INT32_MAX);
        C17_CHECK_EQ(sdiv3(min32), -715827882);
        C17_CHECK_EQ(srem3(min32), -2);
        C17_CHECK_EQ(sdiv3(max32), 715827882);
        C17_CHECK_EQ(srem3(max32), 1);
        C17_CHECK_EQ(sdiv7(min32), -306783378);
        C17_CHECK_EQ(sdiv_m7(min32), 306783378);
        C17_CHECK_EQ(sdiv_m7(max32), -306783378);
        C17_CHECK_EQ(sdiv10(c17_opaque_int(-19)), -1);
        C17_CHECK_EQ(sdiv10(c17_opaque_int(-20)), -2);
        C17_CHECK_EQ(sdiv10(c17_opaque_int(-21)), -2);
    }

    /* 32-bit unsigned constant division/modulo at UINT32_MAX & high-bit values */
    {
        uint32_t umax = (uint32_t)c17_opaque_ull(UINT32_MAX);
        uint32_t uhi = (uint32_t)c17_opaque_ull(0x80000001ULL);
        C17_CHECK_UEQ(udiv3(umax), 1431655765U);
        C17_CHECK_UEQ(urem3(umax), 0U);
        C17_CHECK_UEQ(udiv7(umax), 613566756U);
        C17_CHECK_UEQ(udiv10(umax), 429496729U);
        C17_CHECK_UEQ(udiv3(uhi), 715827883U);
        C17_CHECK_UEQ(urem3(uhi), 0U);
    }

    /* 64-bit signed & unsigned constant division/modulo at extreme values */
    {
        int64_t min64 = c17_opaque_ll(INT64_MIN);
        int64_t max64 = c17_opaque_ll(INT64_MAX);
        uint64_t umax64 = c17_opaque_ull(UINT64_MAX);
        C17_CHECK_EQ(sdiv64_3(min64), -3074457345618258602LL);
        C17_CHECK_EQ(sdiv64_3(max64), 3074457345618258602LL);
        C17_CHECK_EQ(sdiv64_m10(min64), 922337203685477580LL);
        C17_CHECK_EQ(sdiv64_m10(max64), -922337203685477580LL);
        C17_CHECK_UEQ(udiv64_3(umax64), 6148914691236517205ULL);
        C17_CHECK_UEQ(urem64_7(umax64), 1ULL);
        C17_CHECK_UEQ(udiv64_10(umax64), 1844674407370955161ULL);
    }

    /* Signed vs unsigned comparison & chained equality */
    {
        uint32_t u = (uint32_t)c17_opaque_ull(0x80000000ULL);
        C17_CHECK((int32_t)u < 0);
        C17_CHECK(u > 0U);
        int eq_chain = (c17_opaque_int(5) == c17_opaque_int(5)) ==
                       (c17_opaque_int(7) == c17_opaque_int(7));
        C17_CHECK_EQ(eq_chain, 1);
        int ne_chain = (c17_opaque_int(5) == c17_opaque_int(6)) ==
                       (c17_opaque_int(7) == c17_opaque_int(8));
        C17_CHECK_EQ(ne_chain, 1);
    }

    /* Nested ternary select / cmov / csel */
    {
        C17_CHECK_EQ(nested_select(c17_opaque_int(1), c17_opaque_int(2),
                                   c17_opaque_int(5), c17_opaque_int(3),
                                   c17_opaque_int(10), c17_opaque_int(20),
                                   c17_opaque_int(0), c17_opaque_int(0)), 11);
        C17_CHECK_EQ(nested_select(c17_opaque_int(1), c17_opaque_int(2),
                                   c17_opaque_int(3), c17_opaque_int(5),
                                   c17_opaque_int(10), c17_opaque_int(20),
                                   c17_opaque_int(0), c17_opaque_int(0)), 22);
        C17_CHECK_EQ(nested_select(c17_opaque_int(5), c17_opaque_int(2),
                                   c17_opaque_int(0), c17_opaque_int(0),
                                   c17_opaque_int(10), c17_opaque_int(20),
                                   c17_opaque_int(4), c17_opaque_int(4)), 7);
        C17_CHECK_EQ(nested_select(c17_opaque_int(5), c17_opaque_int(2),
                                   c17_opaque_int(0), c17_opaque_int(0),
                                   c17_opaque_int(10), c17_opaque_int(20),
                                   c17_opaque_int(9), c17_opaque_int(4)), 16);
    }

    C17_END();
}
