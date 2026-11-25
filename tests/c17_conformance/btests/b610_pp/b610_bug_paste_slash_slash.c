/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10.3.3p3 — Token pasting '/' ## '/' forms invalid pp-token '//'.
 */
#define CAT(a, b) a ## b
int main(void) {
    int x = CAT(/, /);
    return x;
}
