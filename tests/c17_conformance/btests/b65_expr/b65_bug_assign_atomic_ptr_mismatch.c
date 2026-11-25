/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16.1p1 — Pointer assignment mismatch on _Atomic target.
 */
int main(void) {
    _Atomic int a = 0;
    int *p = &a;
    return *p;
}
