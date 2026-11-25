/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.4.1p5 — Integer constant exceeding ULLONG_MAX.
 */
int main(void) {
    unsigned long long x = 99999999999999999999999999999ULL;
    return x == 0;
}
