/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10 — Real-world preprocessor edge cases from
 * GCC, Clang, Boost.PP, TCC, chibicc, and Wave:
 *   - Function-like macro not followed by '(' is NOT expanded, unless a
 *     subsequent expansion supplies '(' on rescan
 *   - Empty macro arguments & ## placemarker pasting (`a ## empty`, `empty ## b`, `empty ## empty`)
 *   - #line directive updating __LINE__ and __FILE__
 *   - Standard C17 predefined macros (__STDC__, __STDC_VERSION__, __STDC_HOSTED__)
 *   - Character constant arithmetic in #if ('9' - '0' == 9)
 */
#include "c17_test.h"

#define STR1(x) #x
#define XSTR1(x) STR1(x)

#define ADD10(x) ((x) + 10)
#define LPAREN (
#define APPLY(m, a) m a

#define CAT(a, b) a ## b
#define XCAT(a, b) CAT(a, b)
#define EMPTY

#define PICK3(a, b, c) XSTR1(a) ":" XSTR1(b) ":" XSTR1(c)

int main(void) {
    C17_BEGIN("c610_bug_pp_tricky",
              "Deferred fn-macro paren, empty ## placemarkers, #line, C17 predefined");

    /* Standard C17 predefined macros */
    C17_CHECK_EQ(__STDC__, 1);
    C17_CHECK_EQ(__STDC_VERSION__, 201710L);
    C17_CHECK_EQ(__STDC_HOSTED__, 1);

    /* Function-like macro name without '(' stays unexpanded */
    {
        int ADD10 = 77;
        C17_CHECK_EQ(ADD10, 77);
        /* Followed by '(' it expands! */
        C17_CHECK_EQ(ADD10(5), 15);
        /* Deferred invocation through APPLY(ADD10, (25)) */
        C17_CHECK_EQ(APPLY(ADD10, (25)), 35);
    }

    /* Empty macro arguments & ## placemarker rules (C17 §6.10.3.3 p2) */
    {
        int XCAT(val_, EMPTY) = 123;
        int XCAT(EMPTY, _val2) = 456;
        C17_CHECK_EQ(val_, 123);
        C17_CHECK_EQ(_val2, 456);
        C17_CHECK_STR(XSTR1(XCAT(EMPTY, EMPTY)), "");
        C17_CHECK_STR(PICK3(, mid, ), ":mid:");
        C17_CHECK_STR(PICK3(left, , right), "left::right");
    }

    /* #line directive updating __LINE__ and __FILE__ */
    {
#line 500 "virtual_c17_test.c"
        int ln = __LINE__;
        const char *fn = __FILE__;
        C17_CHECK_EQ(ln, 500);
        C17_CHECK_STR(fn, "virtual_c17_test.c");
    }

    /* #if character constant arithmetic guaranteed by C17 §5.2.1 p3 */
    {
        int ok = 0;
#if ('9' - '0') == 9 && ('0' + 5) == '5'
        ok = 1;
#endif
        C17_CHECK_EQ(ok, 1);
    }

    C17_END();
}
