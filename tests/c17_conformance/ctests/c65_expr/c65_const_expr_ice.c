/*
 * C99 §6.6: Integer Constant Expressions (ICE) in array bounds, case labels,
 * enum values, bitfield widths, and static initializers — including casts of
 * floating constants, unevaluated ternary branches, sizeof, and offsetof.
 */
#include "c17_test.h"
#include <stddef.h>

struct Sample {
    char c;
    int arr[3];
};

enum IceTests {
    ICE_CAST   = (int)3.9 - (int)1.9,        /* 3 - 1 = 2 (floating const immediate operand of cast!) */
    ICE_TERN   = (1 ? 10 : (1 / 0)),         /* 10 (dead branch div0 ok in ICE!) */
    ICE_SHORT  = (0 && (1 / 0)) + (1 || (1 / 0)), /* 0 + 1 = 1 */
    ICE_CHAR   = 'Z' - 'A',                  /* 25 */
    ICE_SIZEOF = (int)sizeof(int[4]),        /* 16 */
    ICE_OFF    = (int)offsetof(struct Sample, arr),
};

struct IceBits {
    unsigned int a : (1 ? 5 : 0);
    unsigned int b : (sizeof(short) * 4);
};

/* Array bound from ICE */
static int ice_arr[ICE_CAST + ICE_TERN] = { [ICE_SHORT] = 99 };

static int ice_switch(int x) {
    switch (x) {
    case ICE_CAST:  return 100;
    case ICE_CHAR:  return 200;
    case (int)7.8:  return 300;
    default:        return -1;
    }
}

int main(void) {
    C99_BEGIN("c65_const_expr_ice", "Integer constant expressions in enums, bounds, bitfields, switch");

    C99_CHECK_EQ(ICE_CAST, 2);
    C99_CHECK_EQ(ICE_TERN, 10);
    C99_CHECK_EQ(ICE_SHORT, 1);
    C99_CHECK_EQ(ICE_CHAR, 25);
    C99_CHECK_EQ(ICE_SIZEOF, 16);
    C99_CHECK(ICE_OFF >= 1);
    C99_CHECK_EQ(sizeof(ice_arr), 12 * sizeof(int));
    C99_CHECK_EQ(ice_arr[1], 99);
    C99_CHECK_EQ(ice_switch(2), 100);
    C99_CHECK_EQ(ice_switch(25), 200);
    C99_CHECK_EQ(ice_switch(7), 300);

    struct IceBits ib = { 31u, 255u };
    C99_CHECK_EQ(ib.a, 31u);
    C99_CHECK_EQ(ib.b, 255u);

    C99_END();
}
