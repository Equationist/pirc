/*
 * C17 §6.2.5 p11, §6.10.8.3, & §7.3: _Complex floating-point types.
 * Optional in ISO C11/C17 when __STDC_NO_COMPLEX__ is defined!
 */
#include "c17_test.h"

#ifdef __STDC_NO_COMPLEX__
int main(void) {
    C17_BEGIN("c67_complex_numbers", "C17 optional _Complex (skipped via __STDC_NO_COMPLEX__)");
    C17_NOT_APPLICABLE("__STDC_NO_COMPLEX__ defined per C17 §6.10.8.3");
}
#else
#include <complex.h>

int main(void) {
    C17_BEGIN("c67_complex_numbers", "C17 _Complex arithmetic, promotions, and _Bool conversion");

    double _Complex z1 = 3.0 + 4.0 * I;
    double _Complex z2 = 1.0 - 2.0 * I;

    double _Complex sum = z1 + z2;
    double _Complex prod = z1 * z2; /* (3+4i)(1-2i) = 11 - 2i */
    double _Complex quot = prod / z2;

    C17_CHECK_DBL(creal(sum), 4.0, 1e-12);
    C17_CHECK_DBL(cimag(sum), 2.0, 1e-12);
    C17_CHECK_DBL(creal(prod), 11.0, 1e-12);
    C17_CHECK_DBL(cimag(prod), -2.0, 1e-12);
    C17_CHECK_DBL(creal(quot), 3.0, 1e-12);
    C17_CHECK_DBL(cimag(quot), 4.0, 1e-12);

    double _Complex pure_i = 0.0 + 1.0 * I;
    double _Complex zero_c = 0.0 + 0.0 * I;
    C17_CHECK_EQ((_Bool)pure_i, 1);
    C17_CHECK_EQ((_Bool)zero_c, 0);

    float _Complex fz = 2.0f + 5.0f * I;
    C17_CHECK_DBL(crealf(fz), 2.0, 1e-6);
    C17_CHECK_DBL(cimagf(fz), 5.0, 1e-6);

    C17_END();
}
#endif
