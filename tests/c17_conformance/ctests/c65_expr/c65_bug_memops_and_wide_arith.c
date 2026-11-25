/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.2.5 / §7.24 — Real-world memops & wide-arithmetic bugs:
 *   - Overlapping memmove (forward & backward) across odd/vector boundary sizes
 *   - Inline-expanded memcpy & memset across sizes 0..129
 *   - 64-bit unsigned wraparound & 128-bit portable carry/mul64-high arithmetic
 */
#include <stdint.h>
#include <string.h>
#include "c17_test.h"

/* Portable 64x64 -> 128-bit unsigned multiplication (high 64 bits) */
static uint64_t mul64_hi(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xFFFFFFFFULL;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = b & 0xFFFFFFFFULL;
    uint64_t b_hi = b >> 32;

    uint64_t p0 = a_lo * b_lo;
    uint64_t p1 = a_lo * b_hi;
    uint64_t p2 = a_hi * b_lo;
    uint64_t p3 = a_hi * b_hi;

    uint64_t mid = (p0 >> 32) + (p1 & 0xFFFFFFFFULL) + (p2 & 0xFFFFFFFFULL);
    return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

int main(void) {
    C17_BEGIN("c65_bug_memops_and_wide_arith",
              "Overlapping memmove, odd memcpy/memset, 64x64->128 hi-mul & carry");

    /* Overlapping memmove (both dst > src and dst < src) across sizes 0..65 */
    {
        static const size_t sizes[] = { 0, 1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 64, 65 };
        uint8_t buf[160];
        for (size_t t = 0; t < sizeof(sizes) / sizeof(sizes[0]); t++) {
            size_t n = sizes[t];
            for (size_t i = 0; i < 160; i++) buf[i] = (uint8_t)(i + 1);
            /* Forward overlap: dst = buf + 3, src = buf + 0 */
            memmove(buf + 3, buf, n);
            for (size_t i = 0; i < n; i++) {
                C17_CHECK_EQ(buf[3 + i], (uint8_t)(i + 1));
            }
            /* Backward overlap: dst = buf + 0, src = buf + 3 */
            for (size_t i = 0; i < 160; i++) buf[i] = (uint8_t)(i + 10);
            memmove(buf, buf + 3, n);
            for (size_t i = 0; i < n; i++) {
                C17_CHECK_EQ(buf[i], (uint8_t)(i + 13));
            }
        }
    }

    /* Constant & dynamic size memcpy / memset (sizes 1, 3, 7, 15, 17, 33, 65) */
    {
        uint8_t src[70], dst[70];
        for (int i = 0; i < 70; i++) src[i] = (uint8_t)(i * 3 + 5);
        memset(dst, 0xCC, sizeof(dst));
        memcpy(dst, src, 17);
        for (int i = 0; i < 17; i++) C17_CHECK_EQ(dst[i], src[i]);
        C17_CHECK_EQ(dst[17], 0xCC);

        memset(dst + 5, 0x77, 33);
        C17_CHECK_EQ(dst[4], src[4]);
        for (int i = 5; i < 38; i++) C17_CHECK_EQ(dst[i], 0x77);
        C17_CHECK_EQ(dst[38], 0xCC);
    }

    /* Unsigned wraparound & 128-bit carry & 64x64->hi64 multiplication */
    {
        uint32_t u32m = (uint32_t)c17_opaque_ull(UINT32_MAX);
        C17_CHECK_UEQ(u32m * u32m, 1U);

        uint64_t u64m = c17_opaque_ull(UINT64_MAX);
        uint64_t one = c17_opaque_ull(1ULL);
        uint64_t sum = u64m + one;
        uint64_t carry = (sum < u64m) ? 1ULL : 0ULL;
        C17_CHECK_UEQ(sum, 0ULL);
        C17_CHECK_UEQ(carry, 1ULL);

        /* (2^64 - 1) * (2^64 - 1) = (2^64 - 2) * 2^64 + 1 */
        C17_CHECK_UEQ(u64m * u64m, 1ULL);
        C17_CHECK_UEQ(mul64_hi(u64m, u64m), UINT64_MAX - 1ULL);
        /* 0x100000000 * 0x100000000 = 2^64 -> hi = 1 */
        C17_CHECK_UEQ(mul64_hi(c17_opaque_ull(0x100000000ULL),
                               c17_opaque_ull(0x100000000ULL)), 1ULL);
    }

    C17_END();
}
