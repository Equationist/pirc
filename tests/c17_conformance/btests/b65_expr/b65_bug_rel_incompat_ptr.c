/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.8p2 — Relational comparison of pointers to
 * incompatible object types is a constraint violation.
 * EXPECT-ERROR
 */
int f(int *p, double *q) {
    return p < q;
}
