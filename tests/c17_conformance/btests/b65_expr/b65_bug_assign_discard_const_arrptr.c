/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16.1p1 — Assignment discards const on pointed-to array element.
 */
int main(void) {
    const int a[3] = {1, 2, 3};
    int (*p)[3] = &a;
    return (*p)[0];
}
