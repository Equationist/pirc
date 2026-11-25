/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.6 p3 — Constraint violation:
 *   Constant expressions shall not contain the comma operator (outside
 *   unevaluated operands).
 */
enum E {
    BAD_VAL = (1, 2) /* ERROR: comma operator in integer constant expression */
};

int f(void) {
    return BAD_VAL;
}
