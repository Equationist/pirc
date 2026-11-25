/*
 * C99 §6.3.1.4 & §6.3.1.5: Floating-point <-> integer conversions,
 * truncation toward zero, high-bit uint64_t conversions, and float/double rounding.
 */
#include "c17_test.h"
#include <stdint.h>

int main(void) {
    C99_BEGIN("c63_float_int_conv", "Float <-> integer conversions (truncation, uint64_t high bit)");

    /* Truncation toward zero */
    C99_CHECK_EQ((int)c99_opaque_dbl(1.9999), 1);
    C99_CHECK_EQ((int)c99_opaque_dbl(-1.9999), -1);
    C99_CHECK_EQ((int)c99_opaque_dbl(-0.9999), 0);
    C99_CHECK_EQ((int)c99_opaque_flt(3.75f), 3);
    C99_CHECK_EQ((int)c99_opaque_flt(-3.75f), -3);

    /* Sub-word integer destinations */
    C99_CHECK_EQ((signed char)c99_opaque_dbl(-120.8), -120);
    C99_CHECK_EQ((unsigned char)c99_opaque_dbl(250.9), 250);
    C99_CHECK_EQ((short)c99_opaque_dbl(-32000.7), -32000);
    C99_CHECK_EQ((unsigned short)c99_opaque_dbl(65000.9), 65000);

    /* uint32_t values > INT32_MAX */
    uint32_t u32_hi = (uint32_t)c99_opaque_ull(3000000000ULL);
    double d_u32 = (double)u32_hi;
    C99_CHECK_DBL(d_u32, 3000000000.0, 0.0);
    C99_CHECK_UEQ((uint32_t)c99_opaque_dbl(3000000000.75), 3000000000u);

    /* uint64_t values with bit 63 set (>= 2^63)! */
    uint64_t u64_2p63 = c99_opaque_ull(0x8000000000000000ULL);
    double d_2p63 = (double)u64_2p63;
    C99_CHECK(d_2p63 > 0.0);
    C99_CHECK_DBL(d_2p63, 9223372036854775808.0, 0.0);
    C99_CHECK_UEQ((uint64_t)c99_opaque_dbl(9223372036854775808.0), 0x8000000000000000ULL);

    uint64_t u64_big = c99_opaque_ull(0xC000000000000000ULL);
    double d_big = (double)u64_big;
    C99_CHECK(d_big > d_2p63);
    C99_CHECK_UEQ((uint64_t)c99_opaque_dbl(d_big), 0xC000000000000000ULL);

    /* float <-> uint64_t with bit 63 set */
    float f_2p63 = (float)u64_2p63;
    C99_CHECK(f_2p63 > 0.0f);
    C99_CHECK_UEQ((uint64_t)c99_opaque_flt(f_2p63), 0x8000000000000000ULL);

    /* Signed int64_t negative values <-> double */
    int64_t s64_neg = c99_opaque_ll(-1000000000000LL);
    double d_s64 = (double)s64_neg;
    C99_CHECK_DBL(d_s64, -1000000000000.0, 0.0);
    C99_CHECK_EQ((int64_t)c99_opaque_dbl(d_s64), -1000000000000LL);

    /* Double <-> float exact values and precision truncation */
    double d_exact = c99_opaque_dbl(1.5);
    float f_exact = (float)d_exact;
    C99_CHECK_DBL((double)f_exact, 1.5, 0.0);

    double d_hi_prec = c99_opaque_dbl(1.0 + 1.0 / (double)(1LL << 40));
    float f_trunc = (float)d_hi_prec;
    C99_CHECK(f_trunc == 1.0f); /* 2^-40 lost in float (24-bit significand) */

    C99_END();
}
