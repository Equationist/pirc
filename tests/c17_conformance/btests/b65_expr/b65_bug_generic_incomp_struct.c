/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.1.1p2 — The type name in a generic association
 * shall specify a complete object type (incomplete struct is invalid).
 * EXPECT-ERROR
 */
struct incomp;

int f(int x) {
    return _Generic(x, struct incomp: 1, default: 2);
}
