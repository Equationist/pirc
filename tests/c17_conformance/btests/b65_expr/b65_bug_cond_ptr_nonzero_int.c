/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.15p3 — Conditional operator with pointer and nonzero int.
 */
int main(void) {
    int x = 0;
    int *p = 1 ? &x : 1;
    return p != 0;
}
