/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.2 — Syntax error:
 *   An enum specifier shall have a non-empty enumerator list.
 */
enum Empty {
}; /* ERROR: empty enum is not valid ISO C17 */

int f(void) {
    return 0;
}
