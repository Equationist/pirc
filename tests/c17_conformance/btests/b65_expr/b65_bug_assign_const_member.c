/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16 p2 / §6.3.2.1 p1 — Constraint violation:
 *   A struct containing a const-qualified member is NOT a modifiable lvalue
 *   and cannot be the left operand of assignment.
 */
struct HasConst {
    int a;
    const int b;
};

void f(void) {
    struct HasConst x = { 1, 2 };
    struct HasConst y = { 3, 4 };
    x = y; /* ERROR: struct has const member, not a modifiable lvalue */
}
