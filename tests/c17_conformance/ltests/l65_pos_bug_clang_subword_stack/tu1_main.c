/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Bidirectional slimcc-pir <-> clang ABI
 * interoperability for packed sub-word stack arguments (args 8..13).
 */
#include "c17_test.h"
#include "interop.h"

int64_t slimcc_subword_stack(
    int64_t r0, int64_t r1, int64_t r2, int64_t r3,
    int64_t r4, int64_t r5, int64_t r6, int64_t r7,
    int8_t s0, uint8_t s1, int16_t s2, uint16_t s3, int32_t s4, int64_t s5)
{
    int64_t reg_sum = r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7;
    return reg_sum + (int64_t)s0 * 1000000LL + (int64_t)s1 * 10000LL +
           (int64_t)s2 * 100LL + (int64_t)s3 + (int64_t)s4 + s5;
}

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_subword_stack",
              "Bidirectional slimcc-pir <-> clang sub-word stack args ABI interop");

    int64_t want = (1 + 2 + 3 + 4 + 5 + 6 + 7 + 8) +
                   (-17LL) * 1000000LL + 240LL * 10000LL +
                   (-1234LL) * 100LL + 54321LL + (-987654LL) + 123456789LL;

    /* Direction 1: slimcc-pir caller -> clang callee */
    int64_t r1 = clang_subword_stack(
        c17_opaque_ll(1), c17_opaque_ll(2), c17_opaque_ll(3), c17_opaque_ll(4),
        c17_opaque_ll(5), c17_opaque_ll(6), c17_opaque_ll(7), c17_opaque_ll(8),
        (int8_t)c17_opaque_int(-17),
        (uint8_t)c17_opaque_int(240),
        (int16_t)c17_opaque_int(-1234),
        (uint16_t)c17_opaque_int(54321),
        (int32_t)c17_opaque_int(-987654),
        c17_opaque_ll(123456789LL));
    C17_CHECK_EQ(r1, want);

    /* Direction 2: clang caller -> slimcc-pir callee */
    int64_t r2 = clang_calls_slimcc_subword();
    C17_CHECK_EQ(r2, want);

    C17_END();
}
