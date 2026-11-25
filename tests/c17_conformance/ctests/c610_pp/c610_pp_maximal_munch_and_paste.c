/*
 * C17 §6.4 & §6.10.3: Maximal munch, token pasting multi-char operators and
 * numeric suffixes, function-like macro without '(', #elif short-circuit,
 * and digraph stringification spelling preservation.
 */
#include "c17_test.h"

#define PASTE(a, b) a ## b
#define XPASTE(a, b) PASTE(a, b)
#define STR(x) #x

/* Function-like macro name NOT followed by '(' must NOT expand */
#define FN_OR_VAR(x) ((x) + 1000)

/* #elif after active #if 1 must NOT evaluate #elif expression (even 1/0) */
#if 1
#define ELIF_GUARD 42
#elif (1 / 0)
#define ELIF_GUARD -1
#else
#define ELIF_GUARD -2
#endif

int main(void) {
    C17_BEGIN("c610_pp_maximal_munch_and_paste", "Maximal munch, operator/number ##, fn-macro w/o (, #elif");

    /* Maximal munch: a+++b is (a++) + b; x-->0 is (x--) > 0 */
    int a = 5, b = 10;
    int r1 = a+++b;
    C17_CHECK_EQ(r1, 15);
    C17_CHECK_EQ(a, 6);

    int x = 3, steps = 0;
    while (x-->0) {
        steps++;
    }
    C17_CHECK_EQ(steps, 3);
    C17_CHECK_EQ(x, -1);

    /* Comment acts as token separator; and a / *pd divides by dereferenced ptr */
    int d = 20, div = 4;
    int *pd = &div;
    int q = d / *pd;
    C17_CHECK_EQ(q, 5);
    d /=/*comment*/div;
    C17_CHECK_EQ(d, 5);

    /* Pasting multi-char operators */
    int v = 4;
    v PASTE(<, <=) 2;    /* v <<= 2 -> 16 */
    C17_CHECK_EQ(v, 16);
    v PASTE(+, =) 5;     /* v += 5 -> 21 */
    C17_CHECK_EQ(v, 21);
    v PASTE(-, -);       /* v-- -> 20 */
    C17_CHECK_EQ(v, 20);
    struct { int k; } s = { 77 }, *ps = &s;
    C17_CHECK_EQ(ps PASTE(-, >) k, 77);
    C17_CHECK_EQ((1 PASTE(&, &) 2), 1);
    C17_CHECK_EQ((0 PASTE(|, |) 3), 1);
    C17_CHECK_EQ((5 PASTE(!, =) 6), 1);

    /* Pasting numeric constants & exponents/suffixes (1e+ is ONE pp-number!) */
    C17_CHECK_UEQ(PASTE(123, ULL), 123ULL);
    C17_CHECK_DBL(PASTE(1e+, 2), 100.0, 0.0);
    C17_CHECK_DBL(PASTE(0x1p+, 4), 16.0, 0.0);

    /* Function-like macro without '(' stays an ordinary identifier */
    int FN_OR_VAR = 7;
    C17_CHECK_EQ(FN_OR_VAR + FN_OR_VAR(5), 7 + 1005);

    /* #elif short-circuit */
    C17_CHECK_EQ(ELIF_GUARD, 42);

    /* Digraph spelling preserved by # stringification (§6.10.3.2) */
    C17_CHECK_STR(STR(<:), "<:");
    C17_CHECK_STR(STR(%>), "%>");

    C17_END();
}
