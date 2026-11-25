/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.15p3 / §6.7.6.2p6 — Incompatible array element qualifiers in ?:.
 */
int main(void) {
    const int (*a1)[3] = 0;
    volatile int (*a2)[3] = 0;
    void *p = 1 ? a1 : a2;
    return p != 0;
}
