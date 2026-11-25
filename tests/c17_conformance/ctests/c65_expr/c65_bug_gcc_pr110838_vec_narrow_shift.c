/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.7 — Real-world compiler bug regressions:
 *   - GCC PR110838: loop vectorization over-widening / narrowing of sign-extract
 *     shifts ((int16_t)((int32_t)src[i] >> 31)) on narrow signed arrays
 *   - GCC PR116057: mixed constant/runtime SLP vectorizable array stores
 *   - Runtime shift-by-0 and shift-by-(width-1) edge cases
 */
#include <stdint.h>
#include "c17_test.h"

/* GCC PR110838 pattern */
static void sign_extract_i16(int16_t *dst, const int16_t *src, int n) {
    for (int i = 0; i < n; i++) {
        int32_t v = (int32_t)src[i];
        /* Portable arithmetic sign mask without UB on left shift:
         * note C17 §6.5.7 p5 says right shift of negative signed is
         * implementation-defined (arithmetic on all two's-complement targets);
         * test both unsigned sign-bit extraction and narrow arithmetic ops. */
        uint32_t uv = (uint32_t)v;
        uint32_t mask = 0U - (uv >> 31);
        dst[i] = (int16_t)mask;
    }
}

static void narrow_unsigned_shifts(uint8_t *dst, const uint8_t *src, int n, unsigned sh) {
    for (int i = 0; i < n; i++) {
        dst[i] = (uint8_t)((unsigned)src[i] >> sh);
    }
}

/* GCC PR116057 pattern: mixed constant & runtime lanes in 8-element arrays */
static void mixed_const_runtime(int *out, const int *in, int k) {
    out[0] = 7;
    out[1] = in[1] + k;
    out[2] = 13;
    out[3] = in[3] * 2;
    out[4] = 19;
    out[5] = in[5] - k;
    out[6] = 23;
    out[7] = in[7] ^ k;
}

static uint32_t rshift32(uint32_t x, unsigned s) { return x >> s; }
static uint32_t lshift32(uint32_t x, unsigned s) { return x << s; }
static uint64_t rshift64(uint64_t x, unsigned s) { return x >> s; }
static uint64_t lshift64(uint64_t x, unsigned s) { return x << s; }

int main(void) {
    C17_BEGIN("c65_bug_gcc_pr110838_vec_narrow_shift",
              "GCC PR110838 narrow shifts, PR116057 mixed lanes, boundary shifts");

    /* PR110838 sign extraction on 16-element int16_t array */
    {
        int16_t src[16] = {
            -32768, -1000, -1, 0, 1, 1000, 32767, -2,
            0, -32767, 42, -42, 127, -128, 255, -256
        };
        int16_t dst[16];
        sign_extract_i16(dst, src, c17_opaque_int(16));
        for (int i = 0; i < 16; i++) {
            int16_t want = (src[i] < 0) ? (int16_t)-1 : (int16_t)0;
            C17_CHECK_EQ(dst[i], want);
        }
    }

    /* Narrow unsigned shifts with runtime count 0, 1, 4, 7 */
    {
        uint8_t src[16];
        uint8_t dst[16];
        for (int i = 0; i < 16; i++) {
            src[i] = (uint8_t)(i * 17);
        }
        unsigned counts[4] = { 0, 1, 4, 7 };
        for (int c = 0; c < 4; c++) {
            unsigned sh = (unsigned)c17_opaque_int((int)counts[c]);
            narrow_unsigned_shifts(dst, src, c17_opaque_int(16), sh);
            for (int i = 0; i < 16; i++) {
                C17_CHECK_EQ(dst[i], (uint8_t)(src[i] >> counts[c]));
            }
        }
    }

    /* PR116057 mixed constant/runtime array stores */
    {
        int in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        int out[8] = { 0 };
        mixed_const_runtime(out, in, c17_opaque_int(5));
        C17_CHECK_EQ(out[0], 7);
        C17_CHECK_EQ(out[1], 7);
        C17_CHECK_EQ(out[2], 13);
        C17_CHECK_EQ(out[3], 8);
        C17_CHECK_EQ(out[4], 19);
        C17_CHECK_EQ(out[5], 1);
        C17_CHECK_EQ(out[6], 23);
        C17_CHECK_EQ(out[7], 13);
    }

    /* Boundary shifts by 0 and width-1 */
    {
        uint32_t v32 = (uint32_t)c17_opaque_ull(0x80000001ULL);
        uint64_t v64 = c17_opaque_ull(0x8000000000000001ULL);
        C17_CHECK_UEQ(rshift32(v32, (unsigned)c17_opaque_int(0)), 0x80000001U);
        C17_CHECK_UEQ(lshift32(v32, (unsigned)c17_opaque_int(0)), 0x80000001U);
        C17_CHECK_UEQ(rshift32(v32, (unsigned)c17_opaque_int(31)), 1U);
        C17_CHECK_UEQ(lshift32(v32, (unsigned)c17_opaque_int(31)), 0x80000000U);
        C17_CHECK_UEQ(rshift64(v64, (unsigned)c17_opaque_int(0)), 0x8000000000000001ULL);
        C17_CHECK_UEQ(lshift64(v64, (unsigned)c17_opaque_int(0)), 0x8000000000000001ULL);
        C17_CHECK_UEQ(rshift64(v64, (unsigned)c17_opaque_int(32)), 0x0000000080000000ULL);
        C17_CHECK_UEQ(rshift64(v64, (unsigned)c17_opaque_int(63)), 1ULL);
        C17_CHECK_UEQ(lshift64(v64, (unsigned)c17_opaque_int(63)), 0x8000000000000000ULL);
    }

    C17_END();
}
