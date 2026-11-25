/*
 * C99 §6.4.2.2 (__func__), §6.4.6 (digraphs), and §6.4.9 (// comments).
 */
%:include "c17_test.h"

%:define PASTE_DG(a, b) a %:%: b
%:define STR_DG(x) %:x

static const char *get_func_addr(void) <%
    return __func__;
%>

static int digraph_array_test(void) <%
    int arr<:4:> = <% 11, 22, 33, 44 %>;
    int PASTE_DG(lo, cal) = arr<:2:>;
    return local + arr<:0:>;
%>

static int c99_comment_edge(int a, int b) {
    /* In C99, // starts a line comment, so this is 'a' + 1 (unlike C89 'a / b + 1') */
    int q = a//*comment*/b
            + 1;
    (void)b;
    /* Inside a block comment, // does NOT terminate the block comment */
    int r = /* // */ 5;
    /* Inside string literals, comment delimiters are ordinary characters */
    const char *s = "// /* not a comment */";
    return q + r + (int)strlen(s);
}

int main(void) {
    C99_BEGIN("c64_digraphs_ucn_func", "Digraphs, __func__, and // comment edge cases");

    C99_CHECK_EQ(digraph_array_test(), 44);
    C99_CHECK_STR(STR_DG(hello_digraph), "hello_digraph");

    C99_CHECK_STR(__func__, "main");
    C99_CHECK_EQ(sizeof(__func__), 5);
    C99_CHECK_STR(get_func_addr(), "get_func_addr");
    C99_CHECK(get_func_addr() == get_func_addr());

    C99_CHECK_EQ(c99_comment_edge(20, 4), 20 + 1 + 5 + 22);

    C99_END();
}
