/*
 * C99 §6.5.8, §6.5.9, & Annex F: IEEE 754 floating-point edge cases —
 * NaN unordered comparisons, -0.0 vs +0.0, fneg sign preservation, Inf, subnormals.
 */
#include "c17_test.h"
#include <stdint.h>

static uint64_t dbl_bits(double d) {
    uint64_t u = 0;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static uint32_t flt_bits(float f) {
    uint32_t u = 0;
    memcpy(&u, &f, sizeof(u));
    return u;
}

int main(void) {
    C99_BEGIN("c65_float_ieee_edge", "IEEE 754 NaN comparisons, -0.0/+0.0, Inf, subnormals");

    double z = c99_opaque_dbl(0.0);
    double nz = -z;
    double one = c99_opaque_dbl(1.0);
    double d_nan = z / z;
    double d_pinf = one / z;
    double d_ninf = -one / z;

    /* -0.0 vs +0.0: equal in value, opposite sign bits */
    C99_CHECK(nz == z);
    C99_CHECK(!(nz != z));
    C99_CHECK(!(nz < z));
    C99_CHECK(nz <= z);
    C99_CHECK_UEQ(dbl_bits(z), 0x0000000000000000ULL);
    C99_CHECK_UEQ(dbl_bits(nz), 0x8000000000000000ULL);
    C99_CHECK_UEQ(dbl_bits(-nz), 0x0000000000000000ULL);
    C99_CHECK(one / nz < 0.0);
    C99_CHECK(one / z > 0.0);
    /* -0.0 + +0.0 yields +0.0 under round-to-nearest */
    C99_CHECK_UEQ(dbl_bits(nz + z), 0x0000000000000000ULL);

    /* NaN comparisons: ALL ordered comparisons MUST be false, != MUST be true */
    C99_CHECK(!(d_nan == d_nan));
    C99_CHECK(d_nan != d_nan);
    C99_CHECK(!(d_nan == one));
    C99_CHECK(d_nan != one);
    C99_CHECK(!(d_nan < one));
    C99_CHECK(!(d_nan <= one));
    C99_CHECK(!(d_nan > one));
    C99_CHECK(!(d_nan >= one));
    C99_CHECK(!(one < d_nan));
    C99_CHECK(!(one >= d_nan));
    /* !(x < y) is NOT equivalent to (x >= y) for NaN! */
    C99_CHECK((!(d_nan < one)) == 1);
    C99_CHECK((d_nan >= one) == 0);

    /* Same for float32 */
    float fz = c99_opaque_flt(0.0f);
    float fnz = -fz;
    float fnan = fz / fz;
    C99_CHECK(fnz == fz);
    C99_CHECK_UEQ(flt_bits(fnz), 0x80000000u);
    C99_CHECK(fnan != fnan);
    C99_CHECK(!(fnan == fnan));
    C99_CHECK(!(fnan < 1.0f));
    C99_CHECK(!(fnan >= 1.0f));

    /* Infinities */
    C99_CHECK(d_pinf > 1e308);
    C99_CHECK(d_ninf < -1e308);
    C99_CHECK(d_pinf == d_pinf);
    C99_CHECK(d_pinf != d_ninf);
    double inf_minus_inf = d_pinf - d_pinf;
    C99_CHECK(inf_minus_inf != inf_minus_inf); /* NaN */

    /* Subnormal double arithmetic */
    double min_norm = c99_opaque_dbl(0x1p-1022);
    double sub = min_norm * 0.5;
    C99_CHECK(sub > 0.0);
    C99_CHECK(sub < min_norm);
    C99_CHECK_DBL(sub * 2.0, min_norm, 0.0);

    C99_END();
}
