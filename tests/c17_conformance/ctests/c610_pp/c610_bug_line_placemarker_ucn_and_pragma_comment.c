/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.3 / §6.10.3.1 / §6.10.4 / §6.10.9 —
 * Regression tests for:
 *   1. #line 010 parsed as decimal 10 (not octal 8).
 *   2. ## after empty placemarker does not pre-expand RHS parameter (blue-paint).
 *   3. UCN identifier normalization prior to macro expansion.
 *   4. _Pragma("//") line comment at buffer EOF without over-read.
 */
#include <string.h>
#include "c17_test.h"

static int F_fn(int a, int b) { return a + b; }
#define F_fn(a, b) a ## b

#define a\u00e9 42

#define LN_VAL 500
#define FN_VAL "virt_line_test.c"

int main(void) {
    C17_BEGIN("c610_bug_line_placemarker_ucn_and_pragma_comment",
              "#line decimal 010, ## placemarker blue-paint, UCN macro ident, _Pragma(\"//\")");

    _Pragma("//")
    _Pragma("/* block comment */")

    int blue = F_fn(, F_fn(1, 2));
    C17_CHECK_EQ(blue, 3);

    int u1 = a\u00e9;
    int u2 = aé;
    C17_CHECK_EQ(u1, 42);
    C17_CHECK_EQ(u2, 42);

#line 010
    int l1 = __LINE__;
#line LN_VAL FN_VAL
    int l2 = __LINE__;
    const char *f2 = __FILE__;

    C17_CHECK_EQ(l1, 10);
    C17_CHECK_EQ(l2, 500);
    C17_CHECK(strcmp(f2, "virt_line_test.c") == 0);

    C17_END();
}
