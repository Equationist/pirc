/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §6.8 — Real-world LLVM & GCC optimizer bugs:
 *   - LLVM #79743: nested loop + narrow signed char compound &= + break (SLP/GVN)
 *   - LLVM #116144: nested loop anti-dependence A[i] = A[i+1] + x (LoopInterchange)
 *   - LLVM #241864: AArch64 sbfiz sign-extension of shifted 8-bit/16-bit values
 *   - GCC VRP _Bool loop control (`for (bool h = 0; h < (bool)(b ? -7 : 4); h = 2)`)
 */
#include <stdbool.h>
#include <stdint.h>
#include "c17_test.h"

/* LLVM #79743 (using explicit signed char for strict C17 portability) */
static int l79743_a = 20, l79743_b = 0, l79743_d = 0, l79743_g = 0;
static short l79743_c = -7655;
static signed char l79743_e = 122;
static unsigned l79743_f = 0;

static int l79743_h(void) {
    if (!l79743_a) l79743_a = 1;
    return l79743_a;
}

static void l79743_i(signed char j) {
    l79743_d = 0;
    for (; l79743_d <= 4; l79743_d++) {
        l79743_f = 0;
        for (; (unsigned)(l79743_h() + l79743_c + 2 - -7633) + l79743_f <= 0U; l79743_f++) {
            l79743_g = 1;
            for (; l79743_g <= 7; l79743_g++) {
                signed char *l = &l79743_e;
                l79743_b &= j;
                *l = (signed char)(*l & (signed char)(unsigned char)181U);
                j = *l;
                if ((signed char)(j + 1 - 49) + j) break;
            }
        }
    }
}

/* LLVM #116144: anti-dependence inside nested loop */
static void l116144_anti(int *A, int x, unsigned N, unsigned M) {
    for (unsigned m = 0; m < M; ++m) {
        for (unsigned i = 0U; i < N - 1U; ++i) {
            A[i] = A[i + 1] + x;
        }
    }
}

/* LLVM #241864: AArch64 sbfiz sign-extension of shifted 8-bit/16-bit values */
static int64_t l241864_sext_shl8(uint8_t x) {
    uint8_t s = (uint8_t)(x << 3);
    return (int64_t)(int8_t)s;
}

static int64_t l241864_sext_shl16(uint16_t x) {
    uint16_t s = (uint16_t)(x << 5);
    return (int64_t)(int16_t)s;
}

static int32_t l241864_shl_of_sext8(uint32_t x) {
    int8_t b = (int8_t)(x & 0xFFU);
    return (int32_t)((uint32_t)(int32_t)b << 4);
}

/* GCC VRP _Bool loop control (`h = 2` normalizes to `1`) */
static unsigned long long gcc_bool_loop(unsigned long long a, unsigned char b) {
    unsigned long long result = 0;
    for (bool h = 0; h < (bool)(b ? -7 : 4); h = 2) {
        result += (a == 0) ? 0ULL : (a + b);
    }
    return result;
}

int main(void) {
    C17_BEGIN("c68_bug_llvm_79743_116144_241864",
              "LLVM #79743, #116144, #241864 (AArch64 sbfiz), GCC _Bool loop");

    /* LLVM #79743: inner loop breaks at g == 1 (buggy -O2 gave 8) */
    {
        int k = (0LL < 4073709551613LL);
        l79743_i((signed char)k);
        C17_CHECK_EQ(l79743_g, 1);
    }

    /* LLVM #116144:
     * A = {1,2,3,4,5}, x = 3, M = 3:
     * m=0: {5,6,7,8,5}
     * m=1: {9,10,11,8,5}
     * m=2: {13,14,11,8,5}
     */
    {
        int A[5] = { 1, 2, 3, 4, 5 };
        l116144_anti(A, c17_opaque_int(3), (unsigned)c17_opaque_int(5), (unsigned)c17_opaque_int(3));
        C17_CHECK_EQ(A[0], 13);
        C17_CHECK_EQ(A[1], 14);
        C17_CHECK_EQ(A[2], 11);
        C17_CHECK_EQ(A[3], 8);
        C17_CHECK_EQ(A[4], 5);
    }

    /* LLVM #241864: shifted 8/16-bit sign extensions on AArch64 */
    {
        /* 0x10 << 3 = 0x80 -> (int8_t)0x80 == -128! (NOT +128!) */
        C17_CHECK_EQ(l241864_sext_shl8((uint8_t)c17_opaque_int(0x10)), -128);
        /* 0x3F << 3 = 0xF8 -> (int8_t)0xF8 == -8! */
        C17_CHECK_EQ(l241864_sext_shl8((uint8_t)c17_opaque_int(0x3F)), -8);
        /* 0x0F << 3 = 0x78 -> +120 */
        C17_CHECK_EQ(l241864_sext_shl8((uint8_t)c17_opaque_int(0x0F)), 120);
        /* 0x0400 << 5 = 0x8000 -> (int16_t)0x8000 == -32768! */
        C17_CHECK_EQ(l241864_sext_shl16((uint16_t)c17_opaque_int(0x0400)), -32768);
        /* 0x03FF << 5 = 0x7FE0 -> +32736 */
        C17_CHECK_EQ(l241864_sext_shl16((uint16_t)c17_opaque_int(0x03FF)), 32736);
        /* (int8_t)0xFF == -1, << 4 == -16 */
        C17_CHECK_EQ(l241864_shl_of_sext8((uint32_t)c17_opaque_int(0x123400FF)), -16);
    }

    /* GCC _Bool loop: runs exactly once (h=0 -> body -> h=2 becomes true(1) -> 1<1 false) */
    C17_CHECK_UEQ(gcc_bool_loop(c17_opaque_ull(42ULL), (unsigned char)c17_opaque_int(1)), 43ULL);
    C17_CHECK_UEQ(gcc_bool_loop(c17_opaque_ull(42ULL), (unsigned char)c17_opaque_int(0)), 42ULL);

    C17_END();
}
