/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.4.4 — Unescaped newline in character literal.
 */
int main(void) {
    int c = 'a
';
    return c;
}
