/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.6 p3 — Constraint violation:
 *   Pointer subtraction requires pointers to compatible object types.
 */
#include <stddef.h>

ptrdiff_t f(int *p, double *q) {
    return p - q; /* ERROR: subtracting pointers to incompatible types */
}
