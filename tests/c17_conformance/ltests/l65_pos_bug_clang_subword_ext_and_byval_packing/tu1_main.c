/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Bidirectional slimcc-pir <-> clang ABI interop:
 * sub-word signext/zeroext, odd-sized small byval packing, and union register classification.
 */
#include "c17_test.h"
#include "interop.h"

int8_t slimcc_ret_i8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a + (int32_t)b + (int32_t)c + (int32_t)d + (e ? 1 : 0);
    return (int8_t)(s & 0xff);
}

uint8_t slimcc_ret_u8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a * 2 + (int32_t)b - (int32_t)c + (int32_t)d + (e ? 3 : 0);
    return (uint8_t)(s & 0xff);
}

int16_t slimcc_ret_i16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a * 17 + (int32_t)b * 3 + (int32_t)c - (int32_t)d + (e ? 5 : 0);
    return (int16_t)s;
}

uint16_t slimcc_ret_u16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a + (int32_t)b * 11 - (int32_t)c * 2 + (int32_t)d + (e ? 7 : 0);
    return (uint16_t)s;
}

bool slimcc_ret_bool(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    return ((int32_t)a + (int32_t)b + (int32_t)c + (int32_t)d + (e ? 1 : 0)) != 0;
}

static uint64_t hash_bytes(const uint8_t *p, int n, uint64_t h) {
    for (int i = 0; i < n; i++)
        h = h * 131u + p[i];
    return h;
}

uint64_t slimcc_odd_byval(struct S3 a, struct S5 b, struct S7 c,
                          struct S11 d, struct S13 e, struct S15 f) {
    uint64_t h = 1;
    h = hash_bytes(a.b, 3, h);
    h = hash_bytes(b.b, 5, h);
    h = hash_bytes(c.b, 7, h);
    h = hash_bytes(d.b, 11, h);
    h = hash_bytes(e.b, 13, h);
    h = hash_bytes(f.b, 15, h);
    return h;
}

int64_t slimcc_union_dpair(int64_t r0, int64_t r1, int64_t r2, int64_t r3,
                           int64_t r4, union DoubleOrI64Pair u, int64_t tail) {
    return r0 + r1 + r2 + r3 + r4 + u.p.lo + u.p.hi + tail;
}

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_subword_ext_and_byval_packing",
              "Bidirectional slimcc-pir <-> clang subword signext/zeroext, odd byval, union ABI");

    /* Direction 1: slimcc-pir caller -> clang callee (direct & indirect) */
    int8_t   a = (int8_t)c17_opaque_int(-128);
    uint8_t  b = (uint8_t)c17_opaque_int(255);
    int16_t  c = (int16_t)c17_opaque_int(-32768);
    uint16_t d = (uint16_t)c17_opaque_int(65535);
    bool     e = (c17_opaque_int(1) != 0);

    C17_CHECK_EQ((int)clang_ret_i8(a, b, c, d, e),   (int)slimcc_ret_i8(a, b, c, d, e));
    C17_CHECK_EQ((int)clang_ret_u8(a, b, c, d, e),   (int)slimcc_ret_u8(a, b, c, d, e));
    C17_CHECK_EQ((int)clang_ret_i16(a, b, c, d, e),  (int)slimcc_ret_i16(a, b, c, d, e));
    C17_CHECK_EQ((int)clang_ret_u16(a, b, c, d, e),  (int)slimcc_ret_u16(a, b, c, d, e));
    C17_CHECK_EQ((int)clang_ret_bool(a, b, c, d, e), (int)slimcc_ret_bool(a, b, c, d, e));

    /* Function pointer (call_indirect) with signext/zeroext */
    int16_t (*fp_i16)(int8_t, uint8_t, int16_t, uint16_t, bool) = clang_ret_i16;
    C17_CHECK_EQ((int)fp_i16(-1, 200, -500, 40000, false),
                 (int)slimcc_ret_i16(-1, 200, -500, 40000, false));

    /* Small odd-sized byval structs with surrounding stack canaries */
    volatile uint64_t canary_lo = 0xdeadbeefcafebabeULL;
    struct S3  s3  = { { 1, 2, 3 } };
    struct S5  s5  = { { 4, 5, 6, 7, 8 } };
    struct S7  s7  = { { 9, 10, 11, 12, 13, 14, 15 } };
    struct S11 s11 = { { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 } };
    struct S13 s13 = { { 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39 } };
    struct S15 s15 = { { 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54 } };
    volatile uint64_t canary_hi = 0x0123456789abcdefULL;

    uint64_t got_odd = clang_odd_byval(s3, s5, s7, s11, s13, s15);
    uint64_t exp_odd = slimcc_odd_byval(s3, s5, s7, s11, s13, s15);
    C17_CHECK_EQ(got_odd, exp_odd);
    C17_CHECK_EQ(canary_lo, 0xdeadbeefcafebabeULL);
    C17_CHECK_EQ(canary_hi, 0x0123456789abcdefULL);

    /* Union SSE/INTEGER classification & register exhaustion */
    union DoubleOrI64Pair u;
    u.p.lo = 0x1122334455667788LL;
    u.p.hi = -0x0102030405060708LL;
    int64_t ur1 = clang_union_dpair(1, 2, 3, 4, 5, u, 99);
    int64_t ur2 = slimcc_union_dpair(1, 2, 3, 4, 5, u, 99);
    C17_CHECK_EQ(ur1, ur2);

    /* Direction 2: clang caller -> slimcc-pir callee */
    C17_CHECK_EQ(clang_verify_slimcc_callees(), 0);

    C17_END();
}
