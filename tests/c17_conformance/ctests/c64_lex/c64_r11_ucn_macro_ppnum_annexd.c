/* Round 11 C17 lexer/UCN/pp-number conformance regressions */
#include "c17_test.h"
#include <string.h>

#define S(x) #x
#define XS(x) S(x)
#define \u00e9(x) S(HELLO x)
#define TWO 2

int \u2460 = 1;
int \u2070 = 2;
int \u3004 = 3;
int \u203f = 4;

int main(void) {
    C17_CHECK(strcmp(\u00e9(WORLD), "HELLO WORLD") == 0);
    C17_CHECK(strcmp(XS(1\u00ee+TWO), "1\u00ee+2") == 0);
    const char *bs = XS(\TWO);
    C17_CHECK(strcmp(bs, "\002") == 0);
    C17_CHECK(\u2460 + \u2070 + \u3004 + \u203f == 10);
    C17_END();
}
