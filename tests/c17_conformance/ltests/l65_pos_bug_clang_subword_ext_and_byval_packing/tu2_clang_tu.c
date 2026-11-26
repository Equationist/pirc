/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: bidirectional ABI oracle for sub-word signext/zeroext,
 * odd-sized small byval structs, and union register classification.
 */
#include "interop.h"

int8_t clang_ret_i8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a + (int32_t)b + (int32_t)c + (int32_t)d + (e ? 1 : 0);
    return (int8_t)(s & 0xff);
}

uint8_t clang_ret_u8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a * 2 + (int32_t)b - (int32_t)c + (int32_t)d + (e ? 3 : 0);
    return (uint8_t)(s & 0xff);
}

int16_t clang_ret_i16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a * 17 + (int32_t)b * 3 + (int32_t)c - (int32_t)d + (e ? 5 : 0);
    return (int16_t)s;
}

uint16_t clang_ret_u16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    int32_t s = (int32_t)a + (int32_t)b * 11 - (int32_t)c * 2 + (int32_t)d + (e ? 7 : 0);
    return (uint16_t)s;
}

bool clang_ret_bool(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e) {
    return ((int32_t)a + (int32_t)b + (int32_t)c + (int32_t)d + (e ? 1 : 0)) != 0;
}

static uint64_t hash_bytes(const uint8_t *p, int n, uint64_t h) {
    for (int i = 0; i < n; i++)
        h = h * 131u + p[i];
    return h;
}

uint64_t clang_odd_byval(struct S3 a, struct S5 b, struct S7 c,
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

int64_t clang_union_dpair(int64_t r0, int64_t r1, int64_t r2, int64_t r3,
                          int64_t r4, union DoubleOrI64Pair u, int64_t tail) {
    return r0 + r1 + r2 + r3 + r4 + u.p.lo + u.p.hi + tail;
}

int clang_verify_slimcc_callees(void) {
    int8_t   a = -128;
    uint8_t  b = 255;
    int16_t  c = -32768;
    uint16_t d = 65535;
    bool     e = true;

    if (slimcc_ret_i8(a, b, c, d, e)   != clang_ret_i8(a, b, c, d, e))   return 1;
    if (slimcc_ret_u8(a, b, c, d, e)   != clang_ret_u8(a, b, c, d, e))   return 2;
    if (slimcc_ret_i16(a, b, c, d, e)  != clang_ret_i16(a, b, c, d, e))  return 3;
    if (slimcc_ret_u16(a, b, c, d, e)  != clang_ret_u16(a, b, c, d, e))  return 4;
    if (slimcc_ret_bool(a, b, c, d, e) != clang_ret_bool(a, b, c, d, e)) return 5;
    if (slimcc_ret_bool(-1, 1, 0, 0, false) != false)                    return 6;

    struct S3  s3  = { { 1, 2, 3 } };
    struct S5  s5  = { { 4, 5, 6, 7, 8 } };
    struct S7  s7  = { { 9, 10, 11, 12, 13, 14, 15 } };
    struct S11 s11 = { { 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26 } };
    struct S13 s13 = { { 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39 } };
    struct S15 s15 = { { 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54 } };
    if (slimcc_odd_byval(s3, s5, s7, s11, s13, s15) !=
        clang_odd_byval(s3, s5, s7, s11, s13, s15))
        return 7;

    union DoubleOrI64Pair u;
    u.p.lo = 0x1122334455667788LL;
    u.p.hi = -0x0102030405060708LL;
    if (slimcc_union_dpair(1, 2, 3, 4, 5, u, 99) !=
        clang_union_dpair(1, 2, 3, 4, 5, u, 99))
        return 8;

    return 0;
}
