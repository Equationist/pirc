/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.8p2 — Relational comparison of function
 * pointers is a constraint violation (operands must be pointers to object types).
 * EXPECT-ERROR
 */
int f(int (*a)(void), int (*b)(void)) {
    return a < b;
}
