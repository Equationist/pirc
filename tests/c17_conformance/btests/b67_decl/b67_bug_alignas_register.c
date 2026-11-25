/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.5p2 — An alignment specifier shall not be
 * specified in a declaration of an object with register storage-class.
 * EXPECT-ERROR
 */
void f(void) {
    _Alignas(16) register int x = 1;
    (void)x;
}
