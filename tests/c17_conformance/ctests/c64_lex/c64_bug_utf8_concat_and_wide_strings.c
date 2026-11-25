/*
 * C17 Conformance Test — Class C (§6.4.5 / §6.7.3 / §6.7.9)
 * Adversarial regression for:
 *   1) UTF-8 string literal concatenation: u8"hello" " world" and "hello" u8" world"
 *      MUST yield char[12] (NOT char16_t[] — catches strcmp vs strncmp bug in getStringKind!).
 *   2) Adjacent UTF-16 (u""), UTF-32 (U""), and wide (L"") string concatenation
 *      with unprefixed string literals.
 *   3) Exact-fit non-NUL-terminated char array initialization: char s[3] = "abc";
 *   4) Duplicate identical type qualifiers: const const volatile volatile int (§6.7.3p5).
 */
#include "c17_test.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const const volatile int g_dup_qual = 77;

int main(void) {
    C17_BEGIN("c64_bug_utf8_concat_and_wide_strings",
              "u8/u/U/L string literal concat, exact-fit char[3], duplicate qualifiers");

    /* 1. UTF-8 string literal concatenation MUST produce char[], NOT char16_t[] */
    const char s_u8a[] = u8"hello" " world";
    const char s_u8b[] = "hello" u8" world";
    const char s_u8c[] = u8"foo" u8"bar";
    C17_CHECK(sizeof(s_u8a) == 12);
    C17_CHECK(sizeof(s_u8b) == 12);
    C17_CHECK(sizeof(s_u8c) == 7);
    C17_CHECK(sizeof(u8"hello" " world") == 12);
    C17_CHECK(_Generic(u8"a" "b", char *: 1, default: 2) == 1);
    C17_CHECK(strcmp(s_u8a, "hello world") == 0);
    C17_CHECK(strcmp(s_u8b, "hello world") == 0);
    C17_CHECK(strcmp(s_u8c, "foobar") == 0);

    /* 2. u"", U"", L"" concatenation with unprefixed strings */
    const uint_least16_t s16[] = u"ab" "cd";
    const uint_least32_t s32[] = "xy" U"zw";
    const wchar_t sw[] = L"12" "34";
    C17_CHECK(sizeof(s16) == 5 * sizeof(uint_least16_t));
    C17_CHECK(s16[0] == 'a' && s16[2] == 'c' && s16[3] == 'd' && s16[4] == 0);
    C17_CHECK(sizeof(s32) == 5 * sizeof(uint_least32_t));
    C17_CHECK(s32[0] == 'x' && s32[2] == 'z' && s32[3] == 'w' && s32[4] == 0);
    C17_CHECK(sizeof(sw) == 5 * sizeof(wchar_t));
    C17_CHECK(sw[0] == L'1' && sw[3] == L'4' && sw[4] == 0);

    /* 3. Exact-fit char array without NUL (§6.7.9p14) */
    char exact[3] = "abc";
    C17_CHECK(sizeof(exact) == 3);
    C17_CHECK(exact[0] == 'a' && exact[1] == 'b' && exact[2] == 'c');

    /* 4. Duplicate type qualifiers (§6.7.3p5) */
    const const int local_dup = 55;
    C17_CHECK(g_dup_qual == 77);
    C17_CHECK(local_dup == 55);

    C17_END();
}
