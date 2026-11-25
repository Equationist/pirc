/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.5 p1 — Constraint violation:
 *   The type name in a compound literal shall NOT specify a variable length array.
 */
int f(int n) {
    int *p = (int[n]){ 0 }; /* ERROR: compound literal of VLA type */
    return p[0];
}
