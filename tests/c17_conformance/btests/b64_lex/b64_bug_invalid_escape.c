/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.4.4 — Invalid escape sequence '\q'.
 */
int main(void) {
    const char *s = "\q";
    return s[0];
}
