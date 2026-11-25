/*
 * C99 §6.4.4 & §6.4.5: Hexadecimal floating constants, integer constant
 * typing rules (decimal vs hex/octal), escape sequences, and narrow+wide
 * string literal concatenation.
 */
#include "c17_test.h"
#include <stddef.h>

int main(void) {
    C99_BEGIN("c64_constants_strings", "Hex floats, int constant types, escapes, string concat");

    /* Hexadecimal floating-point constants (§6.4.4.2) */
    double h1 = 0x1.fp+3;      /* (1 + 15/16) * 8 = 15.5 */
    double h2 = 0x.8p0;        /* 0.5 */
    double h3 = 0x10p-2;       /* 16 / 4 = 4.0 */
    double h4 = 0X1.8P+0;      /* 1.5 */
    float  hf = 0x1.4p+2f;     /* 1.25 * 4 = 5.0f */
    double hsub = 0x1p-1022;   /* smallest normal double */
    C99_CHECK_DBL(h1, 15.5, 0.0);
    C99_CHECK_DBL(h2, 0.5, 0.0);
    C99_CHECK_DBL(h3, 4.0, 0.0);
    C99_CHECK_DBL(h4, 1.5, 0.0);
    C99_CHECK_DBL(hf, 5.0, 0.0);
    C99_CHECK(hsub > 0.0);
    C99_CHECK_EQ(sizeof(0x1.0p0f), sizeof(float));
    C99_CHECK_EQ(sizeof(0x1.0p0), sizeof(double));

    /* C99 §6.4.4.1 p5: Integer constant type promotion!
     * Decimal 2147483648 (2^31) has NO unsigned type in its list -> signed long / long long!
     * Hex 0x80000000 DOES have unsigned int in its list -> unsigned int! */
    C99_CHECK((-2147483648) < 0);            /* -(long/llong) is negative! */
    C99_CHECK((0x80000000 - 0x80000001) > 0); /* unsigned int wrap -> positive! */
    C99_CHECK_EQ(sizeof(0x80000000), sizeof(unsigned int));
    C99_CHECK(sizeof(2147483648) >= 8);
    C99_CHECK_EQ(sizeof(1LL), sizeof(long long));
    C99_CHECK_EQ(sizeof(1ULL), sizeof(unsigned long long));
    C99_CHECK_EQ(sizeof(1llu), sizeof(unsigned long long));

    /* Escape sequences in char & string literals */
    C99_CHECK_EQ('\0', 0);
    C99_CHECK_EQ('\7', 7);
    C99_CHECK_EQ('\101', 'A');
    C99_CHECK_EQ('\x41', 'A');
    C99_CHECK_EQ('\x00000042', 'B'); /* Arbitrary number of hex digits in \x escape! */
    C99_CHECK_EQ('\?', '?');
    /* Octal escape takes at most 3 digits: "\1011" is 'A' followed by '1' */
    const char *oct = "\1011";
    C99_CHECK_EQ(oct[0], 'A');
    C99_CHECK_EQ(oct[1], '1');
    C99_CHECK_EQ(oct[2], '\0');

    /* Adjacent string literal concatenation (narrow + narrow, narrow + wide!) */
    const char *cat = "foo" "bar" "baz";
    C99_CHECK_STR(cat, "foobarbaz");
    C99_CHECK_EQ(sizeof("foo" "bar"), 7);

    /* C99 §6.4.5 p4: narrow string + wide string concatenates to wide string */
    const wchar_t *w1 = "ab" L"cd";
    const wchar_t *w2 = L"ab" "cd";
    C99_CHECK_EQ(w1[0], L'a');
    C99_CHECK_EQ(w1[1], L'b');
    C99_CHECK_EQ(w1[2], L'c');
    C99_CHECK_EQ(w1[3], L'd');
    C99_CHECK_EQ(w1[4], L'\0');
    C99_CHECK_EQ(w2[2], L'c');
    C99_CHECK_EQ(sizeof("ab" L"cd"), 5 * sizeof(wchar_t));

    C99_END();
}
