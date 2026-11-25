/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.6.3p1 — A break statement shall appear only in
 * or as a switch body or loop body.
 * EXPECT-ERROR
 */
void f(int x) {
    if (x) {
        break;
    }
}
