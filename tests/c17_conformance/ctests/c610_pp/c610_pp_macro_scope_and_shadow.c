/*
 * C17 §6.10.3: Macro parameter shadowing, zero-arg function-like macro vs
 * object name, unbalanced parens inside string/char macro arguments, null
 * directive '#', and backslash-newline continuation inside macro body.
 */
#include "c17_test.h"

/* Null directive (§6.10.7) */
#

/* Macro parameter name shadows existing object-like macro */
#define SHADOW 999
#define USE_PARAM(SHADOW) ((SHADOW) + 1)

/* Zero-parameter function-like macro vs ordinary variable */
#define ZERO_FN() 42

/* Macro argument containing unbalanced '(' or ')' inside char/string literals */
#define PASS_ARG(x) (x)
#define TAKE_TWO(a, b) ((a) + (int)strlen(b))

/* Multi-line macro body with backslash-newline */
#define MULTILINE_SUM(a, b, c) \
    ((a) + \
     (b) + \
     (c))

int main(void) {
    C17_BEGIN("c610_pp_macro_scope_and_shadow", "Macro param shadow, 0-arg fn macro, paren-in-literal args");

    C17_CHECK_EQ(SHADOW, 999);
    C17_CHECK_EQ(USE_PARAM(5), 6);

    int ZERO_FN = 10;
    C17_CHECK_EQ(ZERO_FN + ZERO_FN(), 52);

    /* Unbalanced parens and commas inside char/string literals in macro args */
    C17_CHECK_EQ(PASS_ARG('('), '(');
    C17_CHECK_EQ(PASS_ARG(')'), ')');
    C17_CHECK_EQ(PASS_ARG(','), ',');
    C17_CHECK_STR(PASS_ARG("((("), "(((");
    C17_CHECK_STR(PASS_ARG("),("), "),(");
    C17_CHECK_EQ(TAKE_TWO('(', "a,b)c"), '(' + 5);

    C17_CHECK_EQ(MULTILINE_SUM(10, 20, 30), 60);

    C17_END();
}
