/* C17 §6.10 / §6.4: Round 9 preprocessor & lexer conformance checks */
#include "c17_test.h"
#include <stddef.h>
#include <string.h>

#if 0
#
endif
int must_stay_skipped_with_dollar = $;
#endif

#define STR(x) #x
#define XSTR(x) STR(x)
#define VA_EXPLICIT_EMPTY(a, ...) a, ## __VA_ARGS__
#define GLUE_PM(a, b) STR(a##b)
#define WRAP_EMPTY(a) a

static int sum2(int a, int b) { return a + b; }

int main(void) {
    /* 1. Null directive in #if 0 and explicit-empty ', ## __VA_ARGS__' */
    int v = sum2(VA_EXPLICIT_EMPTY(10, ) 32);
    C17_CHECK(v == 42);

    /* 2. '$' as Phase-3 pp-token in # stringification */
    C17_CHECK(strcmp(STR($), "$") == 0);

    /* 3. Wide + narrow string concatenation with >0xFF hex escape in narrow piece */
    const wchar_t *ws = L"a" "\x100";
    C17_CHECK(ws[0] == L'a' && ws[1] == (wchar_t)0x100 && ws[2] == 0);

    /* 4. Placemarker whitespace preservation */
    C17_CHECK(strcmp(GLUE_PM(, x), "x") == 0);
    C17_CHECK(strcmp(XSTR(x WRAP_EMPTY()y), "x y") == 0);

    /* 5. C17 Annex D.1 identifier UCNs */
    int \u00A8 = 11;
    int \u00BC = 31;
    C17_CHECK(\u00A8 + \u00BC == 42);

    C17_END();
}
