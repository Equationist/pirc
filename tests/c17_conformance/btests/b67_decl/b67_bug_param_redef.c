/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7 p3 — Constraint violation:
 *   Duplicate parameter identifier in function definition.
 */
int f(int a, int a) { /* ERROR: redefinition of parameter 'a' */
    return a;
}
