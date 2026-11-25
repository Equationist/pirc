/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 p8 — Syntax/constraint violation:
 *   A struct-or-union shall contain at least one member declaration (empty struct
 *   is a GNU extension, not valid ISO C17).
 */
struct Empty {
}; /* ERROR: empty struct is not valid ISO C17 */

int f(void) {
    return 0;
}
