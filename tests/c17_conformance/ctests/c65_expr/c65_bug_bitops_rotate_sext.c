/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.7 / §6.3.1.1 — Real-world bitops & rotate bugs:
 *   - 32-bit & 64-bit rotate idioms including shift-by-0 safe idiom ((-n) & 31)
 *   - 16/32/64-bit byte-swap idioms (bswap recognizer edge cases)
 *   - Sign-extension vs zero-extension chains
 *   - Bitwise complement ~ on uint8_t/uint16_t promotes to signed int (-1!)
 */
#include <stdint.h>
#include "c17_test.h"

static uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << (n & 31U)) | (x >> ((-n) & 31U));
}

static uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> (n & 31U)) | (x << ((-n) & 31U));
}

static uint64_t rotl64(uint64_t x, unsigned n) {
    return (x << (n & 63U)) | (x >> ((-n) & 63U));
}

static uint64_t rotr64(uint64_t x, unsigned n) {
    return (x >> (n & 63U)) | (x << ((-n) & 63U));
}

static uint16_t bswap16(uint16_t x) {
    return (uint16_t)((x >> 8) | (x << 8));
}

static uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FFU) << 24) |
           ((x & 0x0000FF00U) << 8)  |
           ((x & 0x00FF0000U) >> 8)  |
           ((x & 0xFF000000U) >> 24);
}

static uint64_t bswap64(uint64_t x) {
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) << 8)  |
           ((x & 0x000000FF00000000ULL) >> 8)  |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
}

int main(void) {
    C17_BEGIN("c65_bug_bitops_rotate_sext",
              "32/64-bit rotate (incl. 0), bswap, sext/zext chains, ~ promotion");

    /* 32-bit rotate for 0, 1, 7, 16, 31, 32 */
    {
        uint32_t x = (uint32_t)c17_opaque_ull(0x12345678ULL);
        C17_CHECK_UEQ(rotl32(x, (unsigned)c17_opaque_int(0)),  0x12345678U);
        C17_CHECK_UEQ(rotr32(x, (unsigned)c17_opaque_int(0)),  0x12345678U);
        C17_CHECK_UEQ(rotl32(x, (unsigned)c17_opaque_int(4)),  0x23456781U);
        C17_CHECK_UEQ(rotr32(x, (unsigned)c17_opaque_int(4)),  0x81234567U);
        C17_CHECK_UEQ(rotl32(x, (unsigned)c17_opaque_int(16)), 0x56781234U);
        C17_CHECK_UEQ(rotl32(x, (unsigned)c17_opaque_int(31)), 0x091A2B3CU);
        C17_CHECK_UEQ(rotl32(x, (unsigned)c17_opaque_int(32)), 0x12345678U);
        for (unsigned n = 0; n <= 32; n++) {
            unsigned rn = (unsigned)c17_opaque_int((int)n);
            C17_CHECK_UEQ(rotr32(rotl32(x, rn), rn), x);
        }
    }

    /* 64-bit rotate for 0, 1, 8, 32, 63, 64 */
    {
        uint64_t x = c17_opaque_ull(0x0123456789ABCDEFULL);
        C17_CHECK_UEQ(rotl64(x, (unsigned)c17_opaque_int(0)),  0x0123456789ABCDEFULL);
        C17_CHECK_UEQ(rotr64(x, (unsigned)c17_opaque_int(0)),  0x0123456789ABCDEFULL);
        C17_CHECK_UEQ(rotl64(x, (unsigned)c17_opaque_int(8)),  0x23456789ABCDEF01ULL);
        C17_CHECK_UEQ(rotr64(x, (unsigned)c17_opaque_int(8)),  0xEF0123456789ABCDULL);
        C17_CHECK_UEQ(rotl64(x, (unsigned)c17_opaque_int(32)), 0x89ABCDEF01234567ULL);
        C17_CHECK_UEQ(rotr64(rotl64(x, (unsigned)c17_opaque_int(63)),
                             (unsigned)c17_opaque_int(63)), x);
    }

    /* Byte swaps */
    C17_CHECK_UEQ(bswap16((uint16_t)c17_opaque_int(0x1234)), 0x3412U);
    C17_CHECK_UEQ(bswap32((uint32_t)c17_opaque_ull(0x12345678ULL)), 0x78563412U);
    C17_CHECK_UEQ(bswap64(c17_opaque_ull(0x0123456789ABCDEFULL)), 0xEFCDAB8967452301ULL);

    /* Sign-extension & zero-extension chains */
    {
        uint64_t raw = c17_opaque_ull(0x123456789ABC80FFULL);
        C17_CHECK_EQ((int64_t)(int8_t)raw, -1LL);
        C17_CHECK_EQ((int64_t)(int16_t)raw, -32513LL);
        C17_CHECK_EQ((int64_t)(int32_t)raw, (int64_t)(int32_t)0x9ABC80FFU);
        C17_CHECK_UEQ((uint64_t)(uint8_t)raw, 0xFFULL);
        C17_CHECK_UEQ((uint64_t)(uint16_t)raw, 0x80FFULL);
        C17_CHECK_UEQ((uint64_t)(uint32_t)raw, 0x9ABC80FFULL);
    }

    /* Bitwise ~ on uint8_t / uint16_t promotes to signed int! */
    {
        uint8_t z8 = (uint8_t)c17_opaque_int(0);
        uint16_t z16 = (uint16_t)c17_opaque_int(0);
        C17_CHECK_EQ(~z8, -1);
        C17_CHECK_EQ(~z16, -1);
        C17_CHECK((~z8) < 0);
        C17_CHECK_EQ((uint8_t)~z8, 255);
        C17_CHECK_EQ((uint16_t)~z16, 65535);
    }

    C17_END();
}
