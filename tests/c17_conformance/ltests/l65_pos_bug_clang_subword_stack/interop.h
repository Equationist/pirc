/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 — Bidirectional slimcc-pir <-> clang ABI interop
 * for sub-word stack arguments (Darwin ARM64 packs 1B/2B stack args tightly,
 * whereas Linux AAPCS64/SysV x64 use 8B slots!) + variadic default float promotion.
 */
#ifndef L65_SUBWORD_STACK_H
#define L65_SUBWORD_STACK_H

#include <stdint.h>

/* 14 parameters: first 8 in GP regs, last 6 (mixed i8/u8/i16/u16/i32/i64) on stack! */
int64_t clang_subword_stack(
    int64_t r0, int64_t r1, int64_t r2, int64_t r3,
    int64_t r4, int64_t r5, int64_t r6, int64_t r7,
    int8_t s0, uint8_t s1, int16_t s2, uint16_t s3, int32_t s4, int64_t s5);

int64_t slimcc_subword_stack(
    int64_t r0, int64_t r1, int64_t r2, int64_t r3,
    int64_t r4, int64_t r5, int64_t r6, int64_t r7,
    int8_t s0, uint8_t s1, int16_t s2, uint16_t s3, int32_t s4, int64_t s5);

int64_t clang_calls_slimcc_subword(void);

#endif /* L65_SUBWORD_STACK_H */
