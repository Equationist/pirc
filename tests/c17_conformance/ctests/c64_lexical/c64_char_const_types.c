/*
 * C99 §6.4.4.4: Character constants have type int (NOT char!), wide character
 * constants have type wchar_t, and standard simple escape sequences.
 */
#include "c17_test.h"
#include <stddef.h>

int main(void) {
    C99_BEGIN("c64_char_const_types", "Character constant type int, wchar_t, simple escapes");

    /* In C (unlike C++), sizeof('a') MUST be sizeof(int)! */
    C99_CHECK_EQ(sizeof('a'), sizeof(int));
    C99_CHECK_EQ(sizeof('\n'), sizeof(int));
    C99_CHECK_EQ(sizeof(L'a'), sizeof(wchar_t));

    /* All simple escape sequences are distinct nonzero bytes */
    const char esc[] = "\a\b\f\n\r\t\v\\\'\"\?";
    C99_CHECK_EQ(sizeof(esc), 12);
    C99_CHECK_EQ(esc[0], '\a');
    C99_CHECK_EQ(esc[3], '\n');
    C99_CHECK_EQ(esc[7], '\\');
    C99_CHECK_EQ(esc[8], '\'');
    C99_CHECK_EQ(esc[9], '\"');
    C99_CHECK_EQ(esc[10], '?');

    /* Wide string literal length */
    const wchar_t ws[] = L"hello";
    C99_CHECK_EQ(sizeof(ws), 6 * sizeof(wchar_t));
    C99_CHECK_EQ(ws[4], L'o');
    C99_CHECK_EQ(ws[5], L'\0');

    C99_END();
}
