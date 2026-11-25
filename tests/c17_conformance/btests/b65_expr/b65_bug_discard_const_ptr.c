/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16.1 p1 — Constraint violation:
 *   Assigning a pointer-to-const to a pointer-to-non-const discards the
 *   const qualifier.
 */
int *f(const int *cp) {
    int *p = cp; /* ERROR: discards const qualifier from pointer target type */
    return p;
}
