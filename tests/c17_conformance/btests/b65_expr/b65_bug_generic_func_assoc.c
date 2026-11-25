/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1p2 — The type name in a generic association
 * shall specify a complete object type (function type is not an object type).
 * EXPECT-ERROR
 */
int f(int x) {
    return _Generic(x, int(void): 1, default: 2);
}
