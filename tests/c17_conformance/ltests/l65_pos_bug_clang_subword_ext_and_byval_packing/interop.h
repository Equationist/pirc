/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Bidirectional slimcc-pir <-> clang ABI interop:
 *   1. Sub-word (<32-bit) signext/zeroext parameter & return register extension
 *      (int8_t, uint8_t, int16_t, uint16_t, _Bool — direct & indirect calls)
 *   2. Odd-sized small byval structs (3B, 5B, 7B, 9B, 11B, 13B, 15B) with stack
 *      canaries (verifies zero stack buffer overflow in pack_byval_arg)
 *   3. SysV x86-64 union { double d[2]; struct { int64_t a; int64_t b; }; }
 *      and GP/XMM all-or-nothing register exhaustion
 */
#ifndef L65_SUBWORD_EXT_BYVAL_H
#define L65_SUBWORD_EXT_BYVAL_H

#include <stdbool.h>
#include <stdint.h>

struct S3  { uint8_t b[3]; };
struct S5  { uint8_t b[5]; };
struct S7  { uint8_t b[7]; };
struct S11 { uint8_t b[11]; };
struct S13 { uint8_t b[13]; };
struct S15 { uint8_t b[15]; };

union DoubleOrI64Pair {
    double d[2];
    struct { int64_t lo; int64_t hi; } p;
};

/* Direct & indirect sub-word signext/zeroext params & returns */
int8_t   clang_ret_i8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
uint8_t  clang_ret_u8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
int16_t  clang_ret_i16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
uint16_t clang_ret_u16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
bool     clang_ret_bool(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);

int8_t   slimcc_ret_i8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
uint8_t  slimcc_ret_u8(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
int16_t  slimcc_ret_i16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
uint16_t slimcc_ret_u16(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);
bool     slimcc_ret_bool(int8_t a, uint8_t b, int16_t c, uint16_t d, bool e);

/* Small odd-sized byval structs */
uint64_t clang_odd_byval(struct S3 a, struct S5 b, struct S7 c,
                         struct S11 d, struct S13 e, struct S15 f);
uint64_t slimcc_odd_byval(struct S3 a, struct S5 b, struct S7 c,
                          struct S11 d, struct S13 e, struct S15 f);

/* Union SSE/INTEGER classification & register exhaustion */
int64_t clang_union_dpair(int64_t r0, int64_t r1, int64_t r2, int64_t r3,
                          int64_t r4, union DoubleOrI64Pair u, int64_t tail);
int64_t slimcc_union_dpair(int64_t r0, int64_t r1, int64_t r2, int64_t r3,
                           int64_t r4, union DoubleOrI64Pair u, int64_t tail);

int clang_verify_slimcc_callees(void);

#endif /* L65_SUBWORD_EXT_BYVAL_H */
