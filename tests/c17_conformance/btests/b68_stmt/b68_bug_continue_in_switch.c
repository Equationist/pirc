/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.6.2p1 — A continue statement shall appear only
 * in or as a loop body (not directly inside a switch with no enclosing loop).
 * EXPECT-ERROR
 */
int f(int x) {
    switch (x) {
    case 1:
        continue;
    default:
        return 0;
    }
}
