/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: verifies Darwin ARM64 packed sub-word stack arguments
 * (int8_t, uint8_t, int16_t, uint16_t, int32_t, int64_t) in both directions.
 */
#include "interop.h"

int64_t clang_subword_stack(
    int64_t r0, int64_t r1, int64_t r2, int64_t r3,
    int64_t r4, int64_t r5, int64_t r6, int64_t r7,
    int8_t s0, uint8_t s1, int16_t s2, uint16_t s3, int32_t s4, int64_t s5)
{
    int64_t reg_sum = r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7;
    return reg_sum + (int64_t)s0 * 1000000LL + (int64_t)s1 * 10000LL +
           (int64_t)s2 * 100LL + (int64_t)s3 + (int64_t)s4 + s5;
}

int64_t clang_calls_slimcc_subword(void) {
    return slimcc_subword_stack(
        1, 2, 3, 4, 5, 6, 7, 8,
        (int8_t)-17, (uint8_t)240, (int16_t)-1234, (uint16_t)54321,
        (int32_t)-987654, 123456789LL);
}
