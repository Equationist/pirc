/*
 * C17 §6.4.4.4 & §6.4.5: Unicode string & character literals (u8, u, U)
 * and UTF-8/UTF-16/UTF-32 encoding + adjacent string concatenation.
 */
#include "c17_test.h"
#include <stdint.h>

int main(void) {
    C17_BEGIN("c64_unicode_literals", "C17 u8/u/U string and character literals");

    /* u8 UTF-8 string literal */
    const char u8_ascii[] = u8"hello";
    C17_CHECK_EQ(sizeof(u8_ascii), 6);
    C17_CHECK_STR(u8_ascii, "hello");

    /* U+00E9 ('é') encodes in UTF-8 as 0xC3 0xA9 */
    const unsigned char *u8_e = (const unsigned char *)u8"\u00e9";
    C17_CHECK_EQ(sizeof(u8"\u00e9"), 3);
    C17_CHECK_UEQ(u8_e[0], 0xC3u);
    C17_CHECK_UEQ(u8_e[1], 0xA9u);
    C17_CHECK_UEQ(u8_e[2], 0x00u);

    /* Adjacent concatenation: u8 + narrow -> u8 */
    const char u8_cat[] = u8"ab" "cd";
    C17_CHECK_STR(u8_cat, "abcd");

    /* u UTF-16 string & char literals */
    C17_CHECK_EQ(sizeof(u'A'), sizeof(uint_least16_t));
    C17_CHECK_UEQ(u'A', 0x0041u);
    C17_CHECK_UEQ(u'\u03b1', 0x03B1u);
    const uint_least16_t u16s[] = u"A\u00e9";
    C17_CHECK_EQ(sizeof(u16s), 3 * sizeof(uint_least16_t));
    C17_CHECK_UEQ(u16s[0], 0x0041u);
    C17_CHECK_UEQ(u16s[1], 0x00E9u);
    C17_CHECK_UEQ(u16s[2], 0x0000u);

    /* U UTF-32 string & char literals (including supplementary plane U+1F600) */
    C17_CHECK_EQ(sizeof(U'A'), sizeof(uint_least32_t));
    C17_CHECK_UEQ(U'\U0001F600', 0x0001F600u);
    const uint_least32_t u32s[] = U"X" "\U0001F600";
    C17_CHECK_EQ(sizeof(u32s), 3 * sizeof(uint_least32_t));
    C17_CHECK_UEQ(u32s[0], 0x00000058u);
    C17_CHECK_UEQ(u32s[1], 0x0001F600u);
    C17_CHECK_UEQ(u32s[2], 0x00000000u);

    /* UTF-16 surrogate pair for U+1F600: 0xD83D 0xDE00 */
    const uint_least16_t u16_sur[] = u"\U0001F600";
    C17_CHECK_EQ(sizeof(u16_sur), 3 * sizeof(uint_least16_t));
    C17_CHECK_UEQ(u16_sur[0], 0xD83Du);
    C17_CHECK_UEQ(u16_sur[1], 0xDE00u);
    C17_CHECK_UEQ(u16_sur[2], 0x0000u);

    C17_END();
}
