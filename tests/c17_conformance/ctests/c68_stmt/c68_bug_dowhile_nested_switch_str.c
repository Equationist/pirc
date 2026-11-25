/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.5 / §6.4.5 / §6.5.16 — Classic frontend & codegen
 * gotchas from TCC, chibicc, cproc, kefir, GCC, and Clang:
 *   - `continue` inside `do { ... } while (cond)` jumps to `cond`, NOT top!
 *   - Nested `switch` statements with inner `break` and outer fallthrough
 *   - Chained bitfield assignment `x = (s.bf = 0x1234)` yields truncated width!
 *   - Adjacent string literal concatenation with prefixes (u8, L, u, U + plain)
 *   - Recursive function with local static variable + static pointer to static local
 */
#include <stddef.h>
#include <stdint.h>
#include "c17_test.h"

struct bf4 {
    unsigned int f : 4;
    signed int g : 4;
};

static int nested_switch_fn(int a, int b) {
    int r = 0;
    switch (a) {
    case 1:
        r += 10;
        switch (b) {
        case 10: r += 1; break; /* breaks INNER switch only */
        case 20: r += 2; /* fallthrough */
        default: r += 4; break;
        }
        /* fallthrough into outer case 2! */
    case 2:
        r += 100;
        break;
    default:
        r = -1;
        break;
    }
    return r;
}

static int rec_with_static(int n) {
    static int visits = 0;
    static int *const p_visits = &visits;
    (*p_visits)++;
    if (n <= 0) return visits;
    return rec_with_static(n - 1);
}

int main(void) {
    C17_BEGIN("c68_bug_dowhile_nested_switch_str",
              "do-while continue, nested switch, bf assign trunc, wide str concat");

    /* do { ... continue; } while (cond): continue MUST evaluate cond! */
    {
        int iters = 0;
        int cond_evals = 0;
        int k = 0;
        do {
            iters++;
            if (k < 3) {
                k++;
                continue; /* MUST jump to while-condition, NOT skip it! */
            }
            k += 10;
        } while ((cond_evals++, k < 5));
        C17_CHECK_EQ(iters, 4);
        C17_CHECK_EQ(cond_evals, 4);
        C17_CHECK_EQ(k, 13);
    }

    /* Nested switch */
    C17_CHECK_EQ(nested_switch_fn(c17_opaque_int(1), c17_opaque_int(10)), 111);
    C17_CHECK_EQ(nested_switch_fn(c17_opaque_int(1), c17_opaque_int(20)), 116);
    C17_CHECK_EQ(nested_switch_fn(c17_opaque_int(2), c17_opaque_int(10)), 100);

    /* Chained bitfield assignment yields truncated bitfield value (C17 §6.5.16 p3) */
    {
        struct bf4 s = { 0, 0 };
        unsigned r1 = (s.f = (unsigned)c17_opaque_int(0x1235));
        C17_CHECK_UEQ(r1, 5U);
        C17_CHECK_UEQ(s.f, 5U);

        int r2 = (s.g = c17_opaque_int(-1));
        C17_CHECK_EQ(r2, -1);
        C17_CHECK_EQ(s.g, -1);
    }

    /* Adjacent string literal concatenation with encoding prefixes (C17 §6.4.5 p5) */
    {
        const char *u8s = u8"hel" "lo";
        C17_CHECK_STR(u8s, "hello");

        const wchar_t *ws = L"AB" "CD";
        C17_CHECK_EQ(ws[0], L'A');
        C17_CHECK_EQ(ws[3], L'D');
        C17_CHECK_EQ(ws[4], L'\0');

        const uint_least16_t *s16 = "XY" u"Z";
        C17_CHECK_EQ(s16[0], u'X');
        C17_CHECK_EQ(s16[2], u'Z');
        C17_CHECK_EQ(s16[3], 0);

        const uint_least32_t *s32 = U"12" "3";
        C17_CHECK_EQ(s32[0], U'1');
        C17_CHECK_EQ(s32[2], U'3');
        C17_CHECK_EQ(s32[3], 0);
    }

    /* Recursive function with local static + static pointer to local static */
    C17_CHECK_EQ(rec_with_static(c17_opaque_int(5)), 6);
    C17_CHECK_EQ(rec_with_static(c17_opaque_int(2)), 9);

    C17_END();
}
