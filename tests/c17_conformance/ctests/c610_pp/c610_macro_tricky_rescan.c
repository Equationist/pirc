/*
 * C99 §6.10.3: Tricky macro rescanning, deferred expansion, parenthesized
 * comma arguments, #undef/re-#define, and benign identical redefinition.
 */
#include "c17_test.h"

#define DOUBLE(x) ((x) * 2)
#define CALL_DOUBLE DOUBLE

#define EMPTY()
#define DEFER(id) id EMPTY()
#define EXPAND(...) __VA_ARGS__
#define ADD2(a, b) ((a) + (b))

/* Benign identical redefinition (§6.10.3 p2) */
#define BENIGN 1 + 2
#define BENIGN   1   +   2

/* #undef and redefine with different value */
#define MUTABLE 10
static int before_undef(void) { return MUTABLE; }
#undef MUTABLE
#define MUTABLE 20
static int after_undef(void) { return MUTABLE; }

/* Parenthesized commas stay a single macro argument */
#define PICK_SECOND(a, b) (b)
#define STR(x) #x

int main(void) {
    C99_BEGIN("c610_macro_tricky_rescan", "Deferred macro expansion, paren args, undef, benign redef");

    /* Object-like macro expanding to function-like macro name before '(' */
    C99_CHECK_EQ(CALL_DOUBLE(21), 42);

    /* Deferred expansion: first pass emits ADD2(3, 4), EXPAND triggers second pass */
    C99_CHECK_EQ(EXPAND(DEFER(ADD2)(3, 4)), 7);

    /* Benign identical redefinition */
    C99_CHECK_EQ(BENIGN, 3);

    /* #undef and re-#define */
    C99_CHECK_EQ(before_undef(), 10);
    C99_CHECK_EQ(after_undef(), 20);

    /* Commas inside parentheses count as one argument */
    int v = PICK_SECOND((1, 2, 3), 99);
    C99_CHECK_EQ(v, 99);
    C99_CHECK_STR(STR("a, b"), "\"a, b\"");

    C99_END();
}
