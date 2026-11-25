/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.2p1 — If an array size expression is a
 * constant expression, it shall have a value greater than zero.
 * EXPECT-ERROR
 */
void f(void) {
    int a[0];
    (void)a;
}
