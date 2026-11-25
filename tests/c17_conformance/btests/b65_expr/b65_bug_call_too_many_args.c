/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 p2 — Constraint violation:
 *   The number of arguments in a function call shall agree with the number
 *   of parameters in the prototype.
 */
int id1(int a);

int f(void) {
    return id1(1, 2); /* ERROR: too many arguments to function */
}
