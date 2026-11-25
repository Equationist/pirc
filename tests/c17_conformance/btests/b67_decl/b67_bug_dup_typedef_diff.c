/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7 p3 — Constraint violation:
 *   A typedef name may be redefined in the same scope ONLY to denote the
 *   same type, never a different type.
 */
typedef int MyInt;
typedef double MyInt; /* ERROR: typedef redefined with different type */

int f(void) {
    return 0;
}
