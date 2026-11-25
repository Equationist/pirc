/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 p4 — Constraint violation:
 *   The width of a bit-field shall not exceed the width of an object of the
 *   specified type.
 */
struct BadBF {
    unsigned int x : 65; /* ERROR: bit-field width exceeds type width */
};

int f(void) {
    return 0;
}
