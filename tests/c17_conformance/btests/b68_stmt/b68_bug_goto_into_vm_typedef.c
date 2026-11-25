/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.6.1p1 — goto into scope of variably modified typedef.
 */
int main(void) {
    int n = 4;
    goto inside;
    typedef int T[n];
inside:
    return (int)sizeof(T);
}
