/*
 * C99 §6.4.3: Universal Character Names (UCNs) in identifiers and strings.
 */
#include "c17_test.h"

static int caf\u00e9 = 42;
static int \u03b1\u03b2 = 100;

static int f_\u00fc(int x) {
    return x + caf\u00e9;
}

int main(void) {
    C99_BEGIN("c64_ucn_identifiers", "Universal Character Names in identifiers and functions");
    C99_CHECK_EQ(caf\u00e9, 42);
    C99_CHECK_EQ(\u03b1\u03b2, 100);
    C99_CHECK_EQ(f_\u00fc(8), 50);
    C99_END();
}
