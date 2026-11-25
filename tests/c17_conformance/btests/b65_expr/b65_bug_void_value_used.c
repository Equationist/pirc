/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.3.2.2 — Constraint violation:
 *   A void expression cannot be used as a value in initialization/assignment.
 */
void helper(void) {}

int f(void) {
    int x = helper(); /* ERROR: initializing int with void expression */
    return x;
}
