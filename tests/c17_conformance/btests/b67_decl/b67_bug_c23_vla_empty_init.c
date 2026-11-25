/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9p3 — Variable-length array empty '{}' initialization is C23 only.
 */
int main(void) {
    int n = 4;
    int a[n] = {};
    return a[0];
}
