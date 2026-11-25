/*
 * C17 §6.3.1 & §6.5: Signed/unsigned traps — INT_MIN/LLONG_MIN division,
 * unsigned short subtraction promoting to signed int (-1!), ~ on small
 * unsigned types, and unsigned wraparound.
 */
#include "c17_test.h"
#include <limits.h>
#include <stdint.h>

int main(void) {
    C17_BEGIN("c65_signed_unsigned_traps", "INT_MIN/LLONG_MIN div, uint16 sub->int, ~small_uint");

    /* INT_MIN / 1, / 2, % 1, % 3 without overflow */
    int imin = c17_opaque_int(INT_MIN);
    int one = c17_opaque_int(1);
    int two = c17_opaque_int(2);
    int three = c17_opaque_int(3);
    C17_CHECK_EQ(imin / one, INT_MIN);
    C17_CHECK_EQ(imin % one, 0);
    C17_CHECK_EQ(imin / two, -1073741824);
    C17_CHECK_EQ(imin % two, 0);
    C17_CHECK_EQ(imin / three, -715827882);
    C17_CHECK_EQ(imin % three, -2);
    C17_CHECK_UEQ((unsigned int)imin, 0x80000000u);

    /* LLONG_MIN / 1, / 2, % 3 */
    long long llmin = c17_opaque_ll(LLONG_MIN);
    long long ll1 = c17_opaque_ll(1LL);
    long long ll2 = c17_opaque_ll(2LL);
    long long ll3 = c17_opaque_ll(3LL);
    C17_CHECK_EQ(llmin / ll1, LLONG_MIN);
    C17_CHECK_EQ(llmin % ll1, 0LL);
    C17_CHECK_EQ(llmin / ll2, -(1LL << 62));
    C17_CHECK_EQ(llmin % ll3, -2LL);

    /* Classic C gotcha: (uint16_t)0 - (uint16_t)1 promotes to SIGNED int == -1! */
    uint16_t u16_0 = (uint16_t)c17_opaque_int(0);
    uint16_t u16_1 = (uint16_t)c17_opaque_int(1);
    C17_CHECK((u16_0 - u16_1) < 0);
    C17_CHECK_EQ(u16_0 - u16_1, -1);

    /* Bitwise ~ on small unsigned types promotes to int -> -1! */
    uint8_t u8_0 = (uint8_t)c17_opaque_int(0);
    C17_CHECK((~u8_0) < 0);
    C17_CHECK_EQ(~u8_0, -1);
    C17_CHECK((~u16_0) < 0);
    C17_CHECK_EQ(~u16_0, -1);

    /* Unsigned 32-bit multiplication wraparound */
    uint32_t big = (uint32_t)c17_opaque_int(65535);
    C17_CHECK_UEQ(big * big, 4294836225u);

    C17_END();
}
