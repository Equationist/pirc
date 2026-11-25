/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9 p3 — Constraint violation:
 *   A variable length array shall NOT be initialized in C17
 *   (empty `{}` zero-initialization of VLAs was added only in C23).
 */
int f(int n) {
    int arr[n] = { 0 }; /* ERROR: variable-sized object may not be initialized in C17 */
    return arr[0];
}
