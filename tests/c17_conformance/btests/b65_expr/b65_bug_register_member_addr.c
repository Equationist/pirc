/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.3.2p1 & §6.7.1p6 — Taking the address of a
 * member of an object declared with the register storage-class specifier is
 * a constraint violation.
 * EXPECT-ERROR
 */
struct s { int a; int b; };

int *f(void) {
    register struct s v = { 1, 2 };
    return &v.a;
}
