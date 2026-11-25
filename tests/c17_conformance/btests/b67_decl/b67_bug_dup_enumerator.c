/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7 p3 — Constraint violation:
 *   An enumerator identifier cannot be declared twice in the same scope.
 */
enum E1 {
    DUP_ENUM = 1
};

enum E2 {
    DUP_ENUM = 2 /* ERROR: redefinition of enumerator in same scope */
};

int f(void) {
    return DUP_ENUM;
}
