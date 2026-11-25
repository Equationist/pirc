/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.4 p5 — Constraint violation:
 *   A cast expression does not yield an lvalue (GNU cast-as-lvalue extension).
 */
int f(int x) {
    (int)x = 5; /* ERROR: cast expression is not an lvalue */
    return x;
}
