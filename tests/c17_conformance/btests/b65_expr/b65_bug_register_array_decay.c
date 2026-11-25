/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.3.2.1p3 & §6.7.1p6 — An array declared with
 * register storage-class cannot decay to a pointer (its address cannot be
 * computed, including via subscripting).
 * EXPECT-ERROR
 */
int f(void) {
    register int a[3] = { 1, 2, 3 };
    return a[0];
}
