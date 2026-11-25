/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.9.2p2 / §6.7p7 — Constraint violation:
 * External-linkage tentative definition of an incomplete struct type that is
 * never completed before the end of the translation unit must be diagnosed.
 */

struct NeverDefined x;

int dummy_anchor(void) {
    return 0;
}
