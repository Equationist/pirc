/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.5 p2 — Constraint violation:
 *   The operands of the % operator shall have integer type.
 */
double f(double a, double b) {
    return a % b; /* ERROR: invalid operands to binary % */
}
