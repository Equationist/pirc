/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10 — Real-world preprocessor bug regressions:
 *   - Macro producing ## token must NOT perform token pasting on rescan
 *   - Stringizing (#) with quotes, backslashes, and whitespace collapsing
 *   - Dave Prosser's blue-paint / hide-set self-referential macro rules
 *   - #if signed/unsigned intmax_t/uintmax_t arithmetic & short-circuit div-by-0
 */
#include "c17_test.h"

#define STR(x) #x
#define XSTR(x) STR(x)

/* Constructing a ## token via pasting two # tokens: on rescan it is just the
 * punctuator ## and MUST NOT paste its neighbors! */
#define HASH #
#define PASTE(a, b) a ## b
#define XPASTE(a, b) PASTE(a, b)
#define MAKE_HH XPASTE(HASH, HASH)

/* Blue-paint / hide-set mutual recursion */
#define REC_A REC_B
#define REC_B REC_A

/* Self-referential function-like macro: argument is expanded BEFORE body
 * substitution, so F(F(1)) expands inner F(1) -> (1 + f_val(1)), then outer F
 * -> ((1 + f_val(1)) + f_val((1 + f_val(1)))), where inner calls hit the C func! */
static int f_val(int x) {
    return x * 10;
}
#define f_val(x) ((x) + f_val(x))

/* Macro that only expands when followed by '(' */
#define CALL_ME(x) ((x) + 100)
#define CALL_ME_REF CALL_ME

int main(void) {
    C17_BEGIN("c610_bug_pp_rescan_paste",
              "PP rescan non-pasting ##, stringizing escapes, hide-sets, #if");

    /* ## produced by macro expansion must NOT paste adjacent tokens */
    {
        const char *s = XSTR(foo MAKE_HH bar);
        C17_CHECK_STR(s, "foo ## bar");
    }

    /* Stringizing escapes embedded quotes and backslashes, collapses spaces */
    {
        const char *s1 = STR(  hello    world  );
        C17_CHECK_STR(s1, "hello world");

        const char *s2 = STR("a\\b\"c");
        C17_CHECK_STR(s2, "\"a\\\\b\\\"c\"");

        const char *s3 = STR('x' + '\n');
        C17_CHECK_STR(s3, "'x' + '\\n'");
    }

    /* Mutual recursion stops cleanly via hide-set */
    {
        const char *s = XSTR(REC_A);
        C17_CHECK_STR(s, "REC_A");
    }

    /* Self-referential function-like macro nested in its own argument:
     * f_val(3) -> (3 + C_f_val(3)) = 33
     * f_val(f_val(3)) -> (33 + C_f_val(33)) = 363 */
    {
        int r1 = f_val(3);
        int r2 = f_val(f_val(3));
        C17_CHECK_EQ(r1, 33);
        C17_CHECK_EQ(r2, 363);
    }

    /* Object-like macro expanding to function-like macro name followed by (arg) */
    {
        int v = CALL_ME_REF(23);
        C17_CHECK_EQ(v, 123);
    }

    /* #if arithmetic: intmax_t / uintmax_t, undefined ident == 0, short-circuit */
    {
        int pp_ok = 0;
#if (-1 < 0) && !(-1 < 0U)
        pp_ok += 1;
#endif
#if (0xFFFFFFFFU >> 1) == 0x7FFFFFFFU
        pp_ok += 2;
#endif
#if (NEVER_DEFINED_IDENT_XYZ == 0) && (!NEVER_DEFINED_IDENT_XYZ)
        pp_ok += 4;
#endif
#if (1 ? 10 : (1 / 0)) == 10 && (0 ? (1 / 0) : 20) == 20
        pp_ok += 8;
#endif
#if (0 && (1 / 0)) == 0 && (1 || (1 / 0)) == 1
        pp_ok += 16;
#endif
        C17_CHECK_EQ(pp_ok, 31);
    }

    C17_END();
}
