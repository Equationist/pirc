/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.15p3 — Conditional operator between void* and function pointer.
 */
static int fn(void) { return 0; }
int main(void) {
    void *vp = 0;
    void *r = 1 ? vp : fn;
    return r != 0;
}
