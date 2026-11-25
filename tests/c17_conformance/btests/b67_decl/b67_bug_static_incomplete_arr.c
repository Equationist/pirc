/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.9.2 p3 — Constraint violation:
 *   A tentative definition with internal linkage shall NOT have an incomplete type.
 */
static int incomplete_arr[]; /* ERROR: static tentative definition with incomplete type */

int f(void) {
    return 0;
}
