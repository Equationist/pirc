/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1p2 — The type name in a generic association
 * shall specify a complete object type other than a variably modified type.
 * EXPECT-ERROR
 */
int f(int n) {
    return _Generic(n, int[n]: 1, default: 2);
}
