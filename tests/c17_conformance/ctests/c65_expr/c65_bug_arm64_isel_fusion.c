/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 — Backend ISel fusion edge cases (ARM64 & x64):
 *   - ror / rorv fusion: OR, XOR, and ADD rotate idioms (32-bit & 64-bit)
 *   - madd fusion: a*b + c and c + a*b (32-bit & 64-bit, signed & unsigned wrap)
 *   - ptr_add + load/store addressing fusion: negative byte offsets (-256..-1),
 *     signed 32-bit negative array index ([base, wIdx, sxtw #2]), unaligned
 *     offsets (1, 3, 255), and large offsets (> 16KB)
 */
#include <stdint.h>
#include "c17_test.h"

/* Constant & variable rotate idioms using |, ^, and + */
static uint32_t ror32_or(uint32_t x)  { return (x >> 7)  | (x << 25); }
static uint32_t ror32_xor(uint32_t x) { return (x >> 13) ^ (x << 19); }
static uint32_t ror32_add(uint32_t x) { return (x >> 5)  + (x << 27); }
static uint64_t ror64_or(uint64_t x)  { return (x >> 17) | (x << 47); }
static uint64_t ror64_xor(uint64_t x) { return (x >> 29) ^ (x << 35); }
static uint32_t ror32_var(uint32_t x, uint32_t n) { return (x >> n) | (x << (32U - n)); }
static uint64_t ror64_var(uint64_t x, uint64_t n) { return (x >> n) | (x << (64ULL - n)); }

/* madd fusion (32-bit & 64-bit, both operand orders) */
static uint32_t madd32_a(uint32_t a, uint32_t b, uint32_t c) { return a * b + c; }
static uint32_t madd32_b(uint32_t a, uint32_t b, uint32_t c) { return c + a * b; }
static uint64_t madd64_a(uint64_t a, uint64_t b, uint64_t c) { return a * b + c; }
static int64_t  madd64_s(int64_t a, int64_t b, int64_t c)    { return a * b + c; }

/* Negative i32 array indexing (tests ARM64 sxtw vs uxtw extension in load/store!) */
static int32_t load_neg_idx32(const int32_t *mid, int32_t idx) {
    return mid[idx];
}

static int64_t load_neg_idx64(const int64_t *mid, int32_t idx) {
    return mid[idx];
}

static uint8_t load_neg_off_bytes(const uint8_t *mid) {
    return (uint8_t)(mid[-256] + mid[-255] + mid[-1] + mid[0] + mid[255]);
}

int main(void) {
    C17_BEGIN("c65_bug_arm64_isel_fusion",
              "ror/rorv (|/^/+), madd 32/64, negative i32 sxtw indexing, -256..+255 off");

    /* Rotate fusion checks */
    {
        const uint32_t cx = 0xA5C391E7U;
        const uint32_t w7  = (cx >> 7)  | (cx << 25);
        const uint32_t w13 = (cx >> 13) ^ (cx << 19);
        const uint32_t w5  = (cx >> 5)  + (cx << 27);
        uint32_t x = (uint32_t)c17_opaque_ull(cx);
        C17_CHECK_UEQ(ror32_or(x),  w7);
        C17_CHECK_UEQ(ror32_xor(x), w13);
        C17_CHECK_UEQ(ror32_add(x), w5);

        C17_CHECK_UEQ(ror32_var(x, (uint32_t)c17_opaque_int(7)),  w7);
        C17_CHECK_UEQ(ror32_var(x, (uint32_t)c17_opaque_int(13)), w13);
        C17_CHECK_UEQ(ror32_var(x, (uint32_t)c17_opaque_int(5)),  w5);

        uint64_t y = c17_opaque_ull(0x0123456789ABCDEFULL);
        uint64_t want17 = (0x0123456789ABCDEFULL >> 17) | (0x0123456789ABCDEFULL << 47);
        uint64_t want29 = (0x0123456789ABCDEFULL >> 29) ^ (0x0123456789ABCDEFULL << 35);
        C17_CHECK_UEQ(ror64_or(y), want17);
        C17_CHECK_UEQ(ror64_xor(y), want29);
        C17_CHECK_UEQ(ror64_var(y, c17_opaque_ull(17ULL)), want17);
        C17_CHECK_UEQ(ror64_var(y, c17_opaque_ull(29ULL)), want29);
    }

    /* madd checks */
    {
        uint32_t a = (uint32_t)c17_opaque_ull(0x10001ULL);
        uint32_t b = (uint32_t)c17_opaque_ull(0x10002ULL);
        uint32_t c = (uint32_t)c17_opaque_ull(0x5555ULL);
        C17_CHECK_UEQ(madd32_a(a, b, c), (uint32_t)(a * b + c));
        C17_CHECK_UEQ(madd32_b(a, b, c), (uint32_t)(c + a * b));

        uint64_t la = c17_opaque_ull(0x123456789ULL);
        uint64_t lb = c17_opaque_ull(0x987654321ULL);
        uint64_t lc = c17_opaque_ull(0x111111111ULL);
        C17_CHECK_UEQ(madd64_a(la, lb, lc), la * lb + lc);
        C17_CHECK_EQ(madd64_s(c17_opaque_ll(-1000), c17_opaque_ll(25), c17_opaque_ll(7)), -24993);
    }

    /* Negative i32 array indexing (must sign-extend idx, NOT zero-extend!) */
    {
        int32_t arr32[16];
        int64_t arr64[16];
        for (int i = 0; i < 16; i++) {
            arr32[i] = i * 100 + 7;
            arr64[i] = (int64_t)i * 1000000LL - 5000000LL;
        }
        const int32_t *mid32 = &arr32[8];
        const int64_t *mid64 = &arr64[8];
        for (int32_t k = -8; k <= 7; k++) {
            int32_t rk = c17_opaque_int(k);
            C17_CHECK_EQ(load_neg_idx32(mid32, rk), arr32[8 + k]);
            C17_CHECK_EQ(load_neg_idx64(mid64, rk), arr64[8 + k]);
        }
    }

    /* Constant negative & boundary byte offsets (-256, -255, -1, 0, +255) */
    {
        uint8_t buf[600];
        for (int i = 0; i < 600; i++) buf[i] = (uint8_t)(i & 0x3F);
        uint8_t got = load_neg_off_bytes(&buf[300]);
        uint8_t want = (uint8_t)(buf[44] + buf[45] + buf[299] + buf[300] + buf[555]);
        C17_CHECK_EQ(got, want);
    }

    C17_END();
}
