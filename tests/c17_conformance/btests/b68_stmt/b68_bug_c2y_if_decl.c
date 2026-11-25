/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.1 — Declaration in if() condition is a C2y extension.
 */
int main(void) {
    if (int x = 1)
        return 0;
    return 1;
}
