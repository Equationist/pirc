/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10 — Round 3 preprocessor regressions:
 *   1. `defined` inside macro expansion in #if (macro hide-set lock balance)
 *   2. Empty LHS of ## preserving leading whitespace in stringize
 *   3. _Pragma("") inside #if constant expression
 *   4. __FILE__ escaping quotes/backslashes after #line
 */
#include "c17_test.h"
#include <string.h>

#define EMPTY
#define STR(x) #x
#define XSTR(x) STR(x)
#define PASTE_WITH_SPACE(a, b) XSTR(foo a ## b)

#define HAS_FOO 1
#define ID(x) x

#if defined(HAS_FOO) && ID(HAS_FOO)
static const int g_def_ok = 1;
#else
static const int g_def_ok = 0;
#endif

#if _Pragma("") 1
static const int g_pragma_if_ok = 1;
#else
static const int g_pragma_if_ok = 0;
#endif

#line 500 "dir\\sub\"name.c"
static const char g_escaped_file[] = __FILE__;
#line 42 "c610_bug_r3_pp_paste_defined_pragma_and_ucn.c"

int main(void) {
    C17_BEGIN("c610_bug_r3_pp_paste_defined_pragma_and_ucn",
              "defined in macro #if, empty-LHS ## space, _Pragma in #if, #line __FILE__ escaping");

    C17_CHECK_EQ(g_def_ok, 1);
    C17_CHECK_EQ(g_pragma_if_ok, 1);

    /* Empty LHS of ## must preserve the space between `foo` and `bar` */
    const char *s1 = PASTE_WITH_SPACE(, bar);
    C17_CHECK_EQ(strcmp(s1, "foo bar"), 0);

    /* __FILE__ must faithfully preserve backslashes and quotes from #line */
    C17_CHECK_EQ(strcmp(g_escaped_file, "dir\\sub\"name.c"), 0);

    C17_END();
}
