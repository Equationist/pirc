/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9p6 — Negative array designator index.
 */
int main(void) {
    int a[4] = { [-1] = 1 };
    return a[0];
}
