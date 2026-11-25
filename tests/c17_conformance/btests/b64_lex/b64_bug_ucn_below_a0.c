/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.3p2 — UCN below 0xA0 (outside $, @, `) is invalid.
 */
int main(void) {
    const char *s = "\u0041";
    return s[0];
}
