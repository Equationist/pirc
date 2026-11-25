/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2 — Real-world switch codegen bugs:
 *   - 64-bit switch cases differing only in upper 32 bits (no 32-bit truncation!)
 *   - Extreme INT64_MIN / INT64_MAX & INT32_MIN / INT32_MAX jump-table range
 *     subtraction overflow bugs
 *   - Negative dense ranges (-10..-1) with out-of-range inputs
 *   - Switch without default when no case matches
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

static int sw_u64_hi(uint64_t x) {
    switch (x) {
    case 0x0000000000000001ULL: return 1;
    case 0x0000000100000001ULL: return 2;
    case 0x0000000200000001ULL: return 3;
    case 0xFFFFFFFF00000001ULL: return 4;
    default: return -1;
    }
}

static int sw_i64_extreme(int64_t x) {
    switch (x) {
    case INT64_MIN:     return 10;
    case INT64_MIN + 1: return 11;
    case -1LL:          return 12;
    case 0LL:           return 13;
    case INT64_MAX - 1: return 14;
    case INT64_MAX:     return 15;
    default:            return 0;
    }
}

static int sw_i32_extreme(int32_t x) {
    switch (x) {
    case INT32_MIN:     return 100;
    case INT32_MIN + 1: return 101;
    case INT32_MIN + 2: return 102;
    case INT32_MIN + 3: return 103;
    case INT32_MAX:     return 200;
    default:            return -9;
    }
}

static int sw_neg_dense(int x) {
    switch (x) {
    case -10: return 10;
    case -9:  return 9;
    case -8:  return 8;
    case -7:  return 7;
    case -6:  return 6;
    case -5:  return 5;
    case -4:  return 4;
    case -3:  return 3;
    case -2:  return 2;
    case -1:  return 1;
    default:  return 0;
    }
}

static int sw_no_default(int x) {
    int r = 77;
    switch (x) {
    case 1: r = 11; break;
    case 2: r = 22; break;
    case 3: r = 33; break;
    }
    return r;
}

int main(void) {
    C17_BEGIN("c68_bug_switch_edge_cases",
              "64-bit high-half switch, INT_MIN/MAX range overflow, negative dense, no-default");

    /* 64-bit switch cases sharing low 32 bits == 1 */
    C17_CHECK_EQ(sw_u64_hi(c17_opaque_ull(0x0000000000000001ULL)), 1);
    C17_CHECK_EQ(sw_u64_hi(c17_opaque_ull(0x0000000100000001ULL)), 2);
    C17_CHECK_EQ(sw_u64_hi(c17_opaque_ull(0x0000000200000001ULL)), 3);
    C17_CHECK_EQ(sw_u64_hi(c17_opaque_ull(0xFFFFFFFF00000001ULL)), 4);
    C17_CHECK_EQ(sw_u64_hi(c17_opaque_ull(0x0000000300000001ULL)), -1);

    /* INT64_MIN / INT64_MAX range check */
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(INT64_MIN)), 10);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(INT64_MIN + 1)), 11);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(-1LL)), 12);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(0LL)), 13);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(INT64_MAX - 1)), 14);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(INT64_MAX)), 15);
    C17_CHECK_EQ(sw_i64_extreme(c17_opaque_ll(12345LL)), 0);

    /* INT32_MIN / INT32_MAX range check */
    C17_CHECK_EQ(sw_i32_extreme(c17_opaque_int(INT32_MIN)), 100);
    C17_CHECK_EQ(sw_i32_extreme(c17_opaque_int(INT32_MIN + 2)), 102);
    C17_CHECK_EQ(sw_i32_extreme(c17_opaque_int(INT32_MAX)), 200);
    C17_CHECK_EQ(sw_i32_extreme(c17_opaque_int(0)), -9);
    C17_CHECK_EQ(sw_i32_extreme(c17_opaque_int(INT32_MAX - 1)), -9);

    /* Negative dense jump table with below-min & above-max inputs */
    for (int v = -10; v <= -1; v++) {
        C17_CHECK_EQ(sw_neg_dense(c17_opaque_int(v)), -v);
    }
    C17_CHECK_EQ(sw_neg_dense(c17_opaque_int(-11)), 0);
    C17_CHECK_EQ(sw_neg_dense(c17_opaque_int(0)), 0);
    C17_CHECK_EQ(sw_neg_dense(c17_opaque_int(INT32_MIN)), 0);
    C17_CHECK_EQ(sw_neg_dense(c17_opaque_int(INT32_MAX)), 0);

    /* No default */
    C17_CHECK_EQ(sw_no_default(c17_opaque_int(2)), 22);
    C17_CHECK_EQ(sw_no_default(c17_opaque_int(99)), 77);

    C17_END();
}
