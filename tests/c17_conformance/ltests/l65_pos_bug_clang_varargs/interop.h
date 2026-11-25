/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared header for bidirectional slimcc-pir <-> clang variadic struct ABI interop.
 */
#ifndef L65_VARARGS_INTEROP_H
#define L65_VARARGS_INTEROP_H

#include <stdint.h>

struct va_s3  { uint8_t b[3]; };
struct va_f3  { float a, b, c; };
struct va_s12 { uint32_t a, b, c; };
struct va_s17 { uint8_t b[17]; };
struct va_d4  { double a, b, c, d; };

/* Defined in Clang TU, called by slimcc-pir TU */
int clang_va_sum(int n, ...);

/* Defined in slimcc-pir TU, called by Clang TU */
int slimcc_va_sum(int n, ...);

/* Defined in Clang TU, invokes slimcc_va_sum with mixed variadic structs */
int clang_calls_slimcc_va(int n);

#endif /* L65_VARARGS_INTEROP_H */
