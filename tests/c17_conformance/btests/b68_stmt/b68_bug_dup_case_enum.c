/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2p3 — No two case constant expressions in the
 * same switch statement shall have the same value (including via enum aliases).
 * EXPECT-ERROR
 */
enum e { E_A = 5, E_B = 5 };

int f(int x) {
    switch (x) {
    case E_A: return 1;
    case E_B: return 2;
    default:  return 0;
    }
}
