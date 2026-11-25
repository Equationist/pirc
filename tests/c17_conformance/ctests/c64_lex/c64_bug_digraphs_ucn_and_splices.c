/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.4.6p3, §6.4.3, §5.1.1.2 — Digraphs (<:, :>, <%, %>,
 * %:, %:%:), universal character names (\uXXXX, \UXXXXXXXX) in identifiers and
 * strings, and backslash-newline line splices inside identifiers/operators.
 */
#include <string.h>
#include "c17_test.h"

%:define DIGRAPH_PASTE(a, b) a %:%: b
%:define DIGRAPH_STR(x) %:x
%:define DIGRAPH_VAL 42

%:if DIGRAPH_VAL == 42
%:define DIGRAPH_IF_OK 1
%:else
%:define DIGRAPH_IF_OK 0
%:endif

static int digraph_sum(int arr<::>, int n)
<%
    int total = 0;
    for (int i = 0; i < n; i++) <%
        total += arr<:i:>;
    %>
    return total;
%>

/* Universal character names in identifiers (Latin e-acute & Greek alpha) */
static int caf\u00E9_val(int \U000003B1) {
    return \U000003B1 * 3 + 1;
}

int main(void) {
    C17_BEGIN("c64_bug_digraphs_ucn_and_splices",
              "C17 digraphs, %:%: paste, UCN identifiers/strings, line splices");

    /* 1. Digraph directives, paste, stringize, and array/block syntax */
    C17_CHECK_EQ(DIGRAPH_IF_OK, 1);
    {
        int DIGRAPH_PASTE(my_, var) = 77;
        C17_CHECK_EQ(my_var, 77);
        C17_CHECK_EQ(strcmp(DIGRAPH_STR(hello_digraph), "hello_digraph"), 0);

        int nums<:4:> = <% 10, 20, 30, 40 %>;
        C17_CHECK_EQ(digraph_sum(nums, 4), 100);
        C17_CHECK_EQ(nums<:2:>, 30);
    }

    /* 2. Universal character names in identifiers and UTF-8 string literals */
    {
        C17_CHECK_EQ(caf\u00E9_val(c17_opaque_int(9)), 28);
        const char *u8 = "caf\u00E9";
        /* UTF-8 for U+00E9 is 0xC3 0xA9 */
        C17_CHECK_EQ((unsigned char)u8[3], 0xC3u);
        C17_CHECK_EQ((unsigned char)u8[4], 0xA9u);
        C17_CHECK_EQ((unsigned char)u8[5], 0u);
    }

    /* 3. Backslash-newline line splices inside identifiers & multi-char operators */
    {
        int spli\
ced_var = c17_opaque_int(15);
        spli\
ced_var +\
= 5;
        spli\
ced_var <\
<= 2;
        /* (15 + 5) << 2 = 80 */
        C17_CHECK_EQ(spliced_var, 80);
    }

    C17_END();
}
