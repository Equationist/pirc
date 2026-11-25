/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.15 p3 — Constraint violation:
 *   The second and third operands of ?: cannot be pointers to incompatible
 *   object types (neither being void * or a null pointer constant).
 */
void *f(int c, int *p, float *q) {
    return c ? p : q; /* ERROR: incompatible pointer types in ?: */
}
