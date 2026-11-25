/*
 * C99 §6.8.4.2: switch on narrow signed/unsigned integer types (signed char,
 * unsigned char, short, _Bool) with negative & high-bit case values.
 */
#include "c17_test.h"

static int sw_schar(signed char sc) {
    switch (sc) {
    case -128: return 1;
    case -3:   return 33;
    case 0:    return 10;
    case 127:  return 2;
    default:   return -1;
    }
}

static int sw_uchar(unsigned char uc) {
    switch (uc) {
    case 0:   return 10;
    case 253: return 44;
    case 255: return 55;
    default:  return -1;
    }
}

static int sw_short(short sh) {
    switch (sh) {
    case -32768: return 1;
    case -500:   return 2;
    case 32767:  return 3;
    default:     return -1;
    }
}

int main(void) {
    C99_BEGIN("c68_switch_narrow_signed", "switch on signed/unsigned char and short with negative cases");

    C99_CHECK_EQ(sw_schar((signed char)c99_opaque_int(-128)), 1);
    C99_CHECK_EQ(sw_schar((signed char)c99_opaque_int(-3)), 33);
    C99_CHECK_EQ(sw_schar((signed char)c99_opaque_int(0)), 10);
    C99_CHECK_EQ(sw_schar((signed char)c99_opaque_int(127)), 2);

    C99_CHECK_EQ(sw_uchar((unsigned char)c99_opaque_int(0)), 10);
    C99_CHECK_EQ(sw_uchar((unsigned char)c99_opaque_int(253)), 44);
    C99_CHECK_EQ(sw_uchar((unsigned char)c99_opaque_int(255)), 55);

    C99_CHECK_EQ(sw_short((short)c99_opaque_int(-32768)), 1);
    C99_CHECK_EQ(sw_short((short)c99_opaque_int(-500)), 2);
    C99_CHECK_EQ(sw_short((short)c99_opaque_int(32767)), 3);

    C99_END();
}
