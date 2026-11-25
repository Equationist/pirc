/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.9p2 — Equality comparison of pointers to
 * incompatible non-void object types is a constraint violation.
 * EXPECT-ERROR
 */
int f(int *p, double *q) {
    return p == q;
}
