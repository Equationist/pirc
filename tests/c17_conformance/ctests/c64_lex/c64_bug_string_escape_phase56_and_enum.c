/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §5.1.1.2 (phases 5 & 6), §6.4.4.4, §6.7.2.2 —
 * String/char escape sequences across adjacent literal concatenation
 * ("\x41" "2" MUST NOT merge into "\x412"!), 3-digit octal limit ("\0123"),
 * all standard C17 escapes, and extreme INT_MIN/INT_MAX enum constants.
 */
#include <limits.h>
#include <string.h>
#include "c17_test.h"

enum extreme_int_enum {
    EE_MIN = (-2147483647 - 1),
    EE_M1  = -1,
    EE_Z   = 0,
    EE_MAX = 2147483647
};

int main(void) {
    C17_BEGIN("c64_bug_string_escape_phase56_and_enum",
              "Phase 5/6 string concat hex/octal escapes & INT_MIN/INT_MAX enum");

    /* 1. Phase 5 (escapes) happens BEFORE Phase 6 (adjacent string concat)! */
    {
        const char *hex_split = "\x41" "2";
        C17_CHECK_EQ(strlen(hex_split), 2u);
        C17_CHECK_EQ(hex_split[0], 'A');
        C17_CHECK_EQ(hex_split[1], '2');

        const char *oct_split = "\01" "2";
        C17_CHECK_EQ(strlen(oct_split), 2u);
        C17_CHECK_EQ((unsigned char)oct_split[0], 1u);
        C17_CHECK_EQ(oct_split[1], '2');

        /* Octal escape consumes AT MOST 3 octal digits: "\0123" == {'\012', '3'} */
        const char *oct_3 = "\0123";
        C17_CHECK_EQ(strlen(oct_3), 2u);
        C17_CHECK_EQ((unsigned char)oct_3[0], 10u);
        C17_CHECK_EQ(oct_3[1], '3');

        /* Embedded NUL in string literal */
        static const char with_nul[] = "ab\0cd\x00" "ef";
        C17_CHECK_EQ(sizeof(with_nul), 9u);
        C17_CHECK_EQ(with_nul[2], '\0');
        C17_CHECK_EQ(with_nul[3], 'c');
        C17_CHECK_EQ(with_nul[5], '\0');
        C17_CHECK_EQ(with_nul[6], 'e');
    }

    /* 2. Standard C17 character escapes */
    {
        C17_CHECK_EQ('\a', 7);
        C17_CHECK_EQ('\b', 8);
        C17_CHECK_EQ('\t', 9);
        C17_CHECK_EQ('\n', 10);
        C17_CHECK_EQ('\v', 11);
        C17_CHECK_EQ('\f', 12);
        C17_CHECK_EQ('\r', 13);
        C17_CHECK_EQ('\?', '?');
        C17_CHECK_EQ('\'', 39);
        C17_CHECK_EQ('\"', 34);
        C17_CHECK_EQ('\\', 92);
    }

    /* 3. Extreme signed int enum values */
    {
        enum extreme_int_enum e1 = EE_MIN;
        enum extreme_int_enum e2 = EE_MAX;
        C17_CHECK_EQ(e1, INT_MIN);
        C17_CHECK_EQ(e2, INT_MAX);
        C17_CHECK(e1 < EE_M1);
        C17_CHECK(EE_M1 < EE_Z);
        C17_CHECK(EE_Z < e2);
    }

    C17_END();
}
