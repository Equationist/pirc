/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16p2 — Assigning to an array (not a modifiable
 * lvalue) is a constraint violation.
 * EXPECT-ERROR
 */
void f(void) {
    int a[3] = { 1, 2, 3 };
    int b[3] = { 4, 5, 6 };
    a = b;
}
