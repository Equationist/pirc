/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.7p2 — Each operand of a bitwise shift operator
 * shall have integer type (floating-point operand is a constraint violation).
 * EXPECT-ERROR
 */
int f(double x) {
    return (int)(x << 2);
}
