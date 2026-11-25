/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.4.2 — Lexical error:
 *   Hexadecimal floating constant must have at least one hex digit in significand.
 */
double bad_hex_float(void) {
    return 0x.p0; /* ERROR: no hex digits in significand */
}
