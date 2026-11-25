/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.10 — Adversarial preprocessor tests:
 *  - #if 64-bit signed/unsigned arithmetic, shifts, ternary short-circuit
 *  - # stringification escaping \" and \\, whitespace collapse, empty arg
 *  - Variadic macro forwarding, empty __VA_ARGS__, arg counting
 *  - _Pragma inside macro expansion
 *  - Function-like macro suppressed when not followed by '('
 */
#include <string.h>
#include "c17_test.h"

/* 1. #if 64-bit signed vs unsigned arithmetic & short-circuit */
#if (-1 < 0U)
#define PP_U_CMP_OK 0
#else
#define PP_U_CMP_OK 1
#endif

#if (0xFFFFFFFFFFFFFFFFULL > 0) && (((1ULL << 63) >> 63) == 1)
#define PP_U64_OK 1
#else
#define PP_U64_OK 0
#endif

#if (0 ? (1 / 0) : (1 ? 42 : (1 / 0))) == 42
#define PP_TERNARY_OK 1
#else
#define PP_TERNARY_OK 0
#endif

#if (0 && (1 / 0)) || (1 || (1 / 0))
#define PP_SC_OK 1
#else
#define PP_SC_OK 0
#endif

#if ('a' == 97) && ('0' + 9 == '9')
#define PP_CHAR_OK 1
#else
#define PP_CHAR_OK 0
#endif

/* 2. # stringification escaping & whitespace collapse */
#define STR(x) #x
#define XSTR(x) STR(x)

/* 3. Variadic macro forwarding & arg count */
#define VA_COUNT_IMPL(_1, _2, _3, _4, _5, N, ...) N
#define VA_COUNT(...) VA_COUNT_IMPL(__VA_ARGS__, 5, 4, 3, 2, 1, 0)
#define VA_SUM2(a, b) ((a) + (b))
#define VA_CALL(fn, ...) fn(__VA_ARGS__)
#define VA_FWD(...) VA_CALL(VA_SUM2, __VA_ARGS__)
#define VA_TAIL(first, ...) ((first) + 0 __VA_ARGS__)

/* 4. _Pragma inside macro expansion */
#define DO_PRAGMA(x) _Pragma(#x)
#define NOOP_PRAGMA_EXPR(v) (DO_PRAGMA() (v))

/* 5. Function-like macro suppressed when not followed by '(' */
static int dual_fn(int x) {
    return x * 3;
}
#define dual_fn(x) ((x) + 100)

int main(void) {
    C17_BEGIN("c610_bug_pp_if64_stringize_va_pragma",
              "#if 64-bit, # stringize escaping, variadic forwarding, _Pragma, fn-macro");

    /* 1. #if checks */
    C17_CHECK_EQ(PP_U_CMP_OK, 1);
    C17_CHECK_EQ(PP_U64_OK, 1);
    C17_CHECK_EQ(PP_TERNARY_OK, 1);
    C17_CHECK_EQ(PP_SC_OK, 1);
    C17_CHECK_EQ(PP_CHAR_OK, 1);

    /* 2. # stringification */
    C17_CHECK_EQ(strcmp(STR(  a   +   /* comment */   b  ), "a + b"), 0);
    C17_CHECK_EQ(strcmp(STR(), ""), 0);
    C17_CHECK_EQ(strcmp(STR("a\"b\\c"), "\"a\\\"b\\\\c\""), 0);
    C17_CHECK_EQ(strcmp(STR('\n'), "'\\n'"), 0);

    /* 3. Variadic macros */
    C17_CHECK_EQ(VA_COUNT(a), 1);
    C17_CHECK_EQ(VA_COUNT(a, b, c), 3);
    C17_CHECK_EQ(VA_COUNT(a, b, c, d, e), 5);
    C17_CHECK_EQ(VA_FWD(17, 25), 42);
    C17_CHECK_EQ(VA_TAIL(10, ), 10);
    C17_CHECK_EQ(VA_TAIL(10, + 5 + 7), 22);

    /* 4. Empty _Pragma("") inside macro expression */
    C17_CHECK_EQ(NOOP_PRAGMA_EXPR(c17_opaque_int(77)), 77);

    /* 5. Function-like macro vs underlying function */
    C17_CHECK_EQ(dual_fn(5), 105);      /* macro expansion */
    C17_CHECK_EQ((dual_fn)(5), 15);     /* real function call */
    {
        int (*fp)(int) = dual_fn;       /* address of real function (no '(' follows) */
        C17_CHECK_EQ(fp(7), 21);
    }

    C17_END();
}
