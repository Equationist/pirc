/*
 * C17 Conformance Test — Class C (§6.4 / §6.4.8 / §6.10.1 / §6.10.3.3)
 * Adversarial lexer & preprocessor regressions:
 *   1) Maximal munch: a+++b parses as (a++) + b; a---b as (a--) - b;
 *      0x1p+2 is a single hex float pp-number; 0x1e + 1 is 31.
 *   2) Token pasting (##) forming multi-character C17 punctuators:
 *      <<=, >>=, ->, >=, <=, !=, ==, &&, ||, +=, -=.
 *   3) #if character constants and arithmetic expressions.
 *   4) Stringizing (#) whitespace normalization and empty macro arguments.
 */
#include "c17_test.h"
#include <string.h>

#define PASTE(a, b) a ## b
#define PASTE3(a, b, c) a ## b ## c
#define STR(x) #x
#define XSTR(x) STR(x)
#define EMPTY

struct Pair {
    int left;
    int right;
};

#if 'A' == 65 && 'a' - 'A' == 32 && '\n' == 10 && '\0' == 0
#define PP_CHAR_OK 1
#else
#define PP_CHAR_OK 0
#endif

int main(void) {
    C17_BEGIN("c64_bug_pp_maximal_munch_and_paste_ops",
              "Maximal munch, ## punctuator pasting, #if char consts, stringize");

    /* 1. Maximal munch */
    int a = c17_opaque_int(10);
    int b = c17_opaque_int(5);
    int r1 = a+++b; /* (a++) + b == 15, a == 11 */
    C17_CHECK(r1 == 15 && a == 11);
    int r2 = a---b; /* (a--) - b == 6, a == 10 */
    C17_CHECK(r2 == 6 && a == 10);

    double hf = 0x1p+2;
    C17_CHECK(hf == 4.0);
    C17_CHECK((0x1e + 1) == 31);

    /* 2. ## pasting multi-character operators */
    int v = c17_opaque_int(3);
    v PASTE3(<, <, =) 4; /* v <<= 4 -> 48 */
    C17_CHECK(v == 48);
    v PASTE3(>, >, =) 2; /* v >>= 2 -> 12 */
    C17_CHECK(v == 12);
    v PASTE(+, =) 8;     /* v += 8 -> 20 */
    C17_CHECK(v == 20);
    v PASTE(-, =) 5;     /* v -= 5 -> 15 */
    C17_CHECK(v == 15);

    struct Pair pr = { 17, 29 };
    struct Pair *pp = &pr;
    C17_CHECK((pp PASTE(-, >) left) == 17);
    C17_CHECK((pp PASTE(-, >) right) == 29);

    C17_CHECK((v PASTE(>, =) 15) == 1);
    C17_CHECK((v PASTE(<, =) 15) == 1);
    C17_CHECK((v PASTE(=, =) 15) == 1);
    C17_CHECK((v PASTE(!, =) 14) == 1);
    C17_CHECK(((v == 15) PASTE(&, &) (a == 10)) == 1);
    C17_CHECK(((v == 0)  PASTE(|, |) (a == 10)) == 1);

    /* 3. #if character constants */
    C17_CHECK(PP_CHAR_OK == 1);

    /* 4. Stringizing whitespace collapsing & empty args */
    C17_CHECK(strcmp(STR(   hello    world   ), "hello world") == 0);
    C17_CHECK(strcmp(XSTR(EMPTY), "") == 0);
    C17_CHECK(strcmp(STR(a +   b), "a + b") == 0);

    C17_END();
}
