/*
 * C17 §6.4.4 & §6.4.5: Octal vs decimal constants, hex-E + space gotcha,
 * embedded NUL bytes in string literals, hex escape stopping via adjacent
 * literal splitting, and zero-padding of partial char array string init.
 */
#include "c17_test.h"

int main(void) {
    C17_BEGIN("c64_pp_number_and_str_gotchas", "Octal, hex-E spacing, embedded NUL, hex escape split, char[] pad");

    /* Octal vs decimal */
    C17_CHECK_EQ(010, 8);
    C17_CHECK_EQ(077, 63);
    C17_CHECK_EQ(0x1e + 2, 32);
    C17_CHECK_DBL(0x1p+2, 4.0, 0.0);

    /* Embedded NUL inside string literal */
    const char emb[] = "abc\0def";
    C17_CHECK_EQ(sizeof(emb), 8);
    C17_CHECK_EQ(strlen(emb), 3);
    C17_CHECK_EQ(emb[3], '\0');
    C17_CHECK_EQ(emb[4], 'd');
    C17_CHECK_EQ(emb[6], 'f');
    C17_CHECK_EQ(emb[7], '\0');

    /* Hex escape stops at non-hex char or string literal boundary */
    const char *s1 = "\x41G";
    C17_CHECK_EQ(s1[0], 'A');
    C17_CHECK_EQ(s1[1], 'G');
    const char *s2 = "\x41" "1";
    C17_CHECK_EQ(s2[0], 'A');
    C17_CHECK_EQ(s2[1], '1');
    C17_CHECK_EQ(s2[2], '\0');

    /* Partial char array init MUST zero-fill all remaining bytes (§6.7.9 p21) */
    char buf[8] = "hi";
    C17_CHECK_EQ(buf[0], 'h');
    C17_CHECK_EQ(buf[1], 'i');
    for (int i = 2; i < 8; i++) {
        C17_CHECK_EQ(buf[i], '\0');
    }

    /* Exact-fit char array init (no NUL) */
    char exact[4] = "WXYZ";
    C17_CHECK_EQ(sizeof(exact), 4);
    C17_CHECK_EQ(exact[0], 'W');
    C17_CHECK_EQ(exact[3], 'Z');

    C17_END();
}
