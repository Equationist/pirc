/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.10 — _Static_assert is a declaration, not an expression.
 */
int main(void) {
    int x = (_Static_assert(1, ""), 0);
    return x;
}
