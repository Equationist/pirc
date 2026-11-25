/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1 — The controlling operand of _Generic must
 * be an assignment-expression in C17 (type-name is not valid C17).
 * EXPECT-ERROR
 */
int f(void) {
    return _Generic(int, int: 1, default: 2);
}
