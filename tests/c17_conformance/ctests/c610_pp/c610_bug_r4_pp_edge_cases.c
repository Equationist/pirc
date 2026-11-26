/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10.9 / §6.4.2.1 — Round 4 preprocessor & lexer regressions:
 *   1. _Pragma("once") expanded inside a macro prevents duplicate header inclusion
 *   2. Skipped #if 0 branches tolerate invalid/empty char literals ('' and L'ab')
 *   3. C17 Annex D superscript digits (U+00B2, U+00B3, U+00B9) in identifiers
 */
#include "c17_test.h"
#include "c610_r4_once.h"
#include "c610_r4_once.h"

#if 0
int bad1 = '';
int bad2 = L'ab';
int bad3 = u'cd';
#endif

static int calc_sq(int v\u00B2, int v\u00B3, int v\u00B9) {
    return v\u00B2 + v\u00B3 + v\u00B9;
}

int main(void) {
    C17_CHECK(r4_once_counter == 42);
    C17_CHECK(calc_sq(4, 8, 2) == 14);
    C17_END();
}
