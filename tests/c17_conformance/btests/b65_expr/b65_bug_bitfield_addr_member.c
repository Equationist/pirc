/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.3.2 p1 — Constraint violation:
 *   The & operator shall not be applied to a bit-field (even parenthesized).
 */
struct BF {
    unsigned x : 5;
};

void *f(struct BF *p) {
    return &(p->x); /* ERROR: cannot take address of bit-field */
}
