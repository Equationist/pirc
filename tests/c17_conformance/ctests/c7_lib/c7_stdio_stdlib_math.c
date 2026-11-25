/*
 * C99 §7.12 (<math.h>), §7.19 (<stdio.h> snprintf), §7.20 (<stdlib.h> C99 fns).
 */
#include "c17_test.h"
#include <math.h>
#include <stdlib.h>

int main(void) {
    C99_BEGIN("c7_stdio_stdlib_math", "C99 snprintf length, strtoll/llabs/lldiv, math macros");

    /* §7.19.6.5: snprintf(NULL, 0, ...) returns exact length needed (excluding NUL) */
    int needed = snprintf(NULL, 0, "val=%d:%s", 12345, "ok");
    C99_CHECK_EQ(needed, 12);

    /* Truncation always NUL-terminates and still returns full needed length */
    char small[6];
    int n2 = snprintf(small, sizeof(small), "abcdefghij");
    C99_CHECK_EQ(n2, 10);
    C99_CHECK_STR(small, "abcde");

    /* §7.20: strtoll, strtoull, llabs, lldiv */
    char *end = NULL;
    long long vll = strtoll("-1234567890123xyz", &end, 10);
    C99_CHECK_EQ(vll, -1234567890123LL);
    C99_CHECK_STR(end, "xyz");

    unsigned long long vull = strtoull("0xdeadbeefcafe", &end, 0);
    C99_CHECK_UEQ(vull, 0xdeadbeefcafeULL);
    C99_CHECK_EQ(*end, '\0');

    C99_CHECK_EQ(llabs(-9876543210LL), 9876543210LL);
    lldiv_t q = lldiv(-17LL, 5LL);
    C99_CHECK_EQ(q.quot, -3LL);
    C99_CHECK_EQ(q.rem, -2LL);

    /* §7.12: C99 math classification macros & functions */
    double d_nan = nan("");
    double d_inf = c99_opaque_dbl(1.0) / c99_opaque_dbl(0.0);
    double d_nz = -c99_opaque_dbl(0.0);

    C99_CHECK(isnan(d_nan) != 0);
    C99_CHECK(isnan(1.0) == 0);
    C99_CHECK(isinf(d_inf) != 0);
    C99_CHECK(isfinite(3.14) != 0);
    C99_CHECK(isfinite(d_inf) == 0);
    C99_CHECK(signbit(d_nz) != 0);
    C99_CHECK(signbit(0.0) == 0);
    C99_CHECK_DBL(copysign(5.0, -1.0), -5.0, 0.0);
    C99_CHECK_DBL(trunc(-2.9), -2.0, 0.0);
    C99_CHECK_DBL(round(2.5), 3.0, 0.0);
    C99_CHECK_DBL(fma(3.0, 4.0, 5.0), 17.0, 0.0);

    C99_END();
}
