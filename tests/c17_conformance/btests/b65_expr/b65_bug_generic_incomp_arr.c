/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1p2 — The type name in a generic association
 * shall specify a complete object type (incomplete array type is invalid).
 * EXPECT-ERROR
 */
int f(int x) {
    return _Generic(x, int[]: 1, default: 2);
}
