/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.6.3 p10 — Constraint violation:
 *   A void parameter must be the sole unnamed parameter in a parameter list.
 */
int bad_fn(int a, void); /* ERROR: void cannot be combined with other parameters */

int g(void) {
    return 0;
}
