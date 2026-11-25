/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.4 p1 — Constraint violation:
 *   The operand of postfix ++ shall be a modifiable lvalue (not const).
 */
int f(void) {
    const int x = 5;
    x++; /* ERROR: increment of read-only variable */
    return x;
}
