/*
 * C99 §6.3.1.2 & §6.5.3.1: _Bool conversions from all scalar types
 * (integers with zero low bytes, fractional floats, -0.0, NaN, pointers),
 * increment/decrement, compound assignment, parameter & return normalization.
 */
#include "c17_test.h"
#include <stdint.h>

static _Bool ret_bool_from_int(int x) {
    return x;
}

static _Bool ret_bool_from_dbl(double d) {
    return d;
}

static int take_bool(_Bool b) {
    return (int)b;
}

int main(void) {
    C99_BEGIN("c63_bool_conversions", "_Bool conversions, NaN/-0.0, ++/--, ABI normalization");

    /* Nonzero integers whose low byte / low 32 bits are 0 MUST convert to 1! */
    int v256 = c99_opaque_int(256);
    int v65536 = c99_opaque_int(65536);
    unsigned long long vhi = c99_opaque_ull(1ULL << 40);
    _Bool b1 = v256;
    _Bool b2 = v65536;
    _Bool b3 = vhi;
    _Bool b0 = c99_opaque_int(0);
    C99_CHECK_EQ(b1, 1);
    C99_CHECK_EQ(b2, 1);
    C99_CHECK_EQ(b3, 1);
    C99_CHECK_EQ(b0, 0);

    /* Fractional floats, -0.0, +0.0, NaN, Inf */
    double tiny = c99_opaque_dbl(0.1);
    double neg_tiny = c99_opaque_dbl(-0.0001);
    double neg_zero = -c99_opaque_dbl(0.0);
    double pos_zero = c99_opaque_dbl(0.0);
    double d_nan = c99_opaque_dbl(0.0) / c99_opaque_dbl(0.0);
    double d_inf = c99_opaque_dbl(1.0) / c99_opaque_dbl(0.0);

    C99_CHECK_EQ((_Bool)tiny, 1);
    C99_CHECK_EQ((_Bool)neg_tiny, 1);
    C99_CHECK_EQ((_Bool)neg_zero, 0);
    C99_CHECK_EQ((_Bool)pos_zero, 0);
    C99_CHECK_EQ((_Bool)d_nan, 1);
    C99_CHECK_EQ((_Bool)d_inf, 1);

    float ftiny = c99_opaque_flt(0.25f);
    float fnz = -c99_opaque_flt(0.0f);
    C99_CHECK_EQ((_Bool)ftiny, 1);
    C99_CHECK_EQ((_Bool)fnz, 0);

    /* Pointers -> _Bool */
    int dummy = 0;
    void *p_nonnull = &dummy;
    void *p_null = NULL;
    C99_CHECK_EQ((_Bool)p_nonnull, 1);
    C99_CHECK_EQ((_Bool)p_null, 0);

    /* Increment, decrement, and compound assignment on _Bool (§6.5.3.1, §6.5.16.2) */
    _Bool b = 0;
    C99_CHECK_EQ(++b, 1);
    C99_CHECK_EQ(b, 1);
    C99_CHECK_EQ(++b, 1); /* 1 + 1 = 2 -> converts to _Bool -> 1! */
    C99_CHECK_EQ(b, 1);
    C99_CHECK_EQ(b++, 1);
    C99_CHECK_EQ(b, 1);
    C99_CHECK_EQ(--b, 0);
    C99_CHECK_EQ(b, 0);
    C99_CHECK_EQ(--b, 1); /* 0 - 1 = -1 -> converts to _Bool -> 1! */
    C99_CHECK_EQ(b, 1);

    b = 1;
    b += 255; /* 256 -> _Bool -> 1! */
    C99_CHECK_EQ(b, 1);
    b -= 1;
    C99_CHECK_EQ(b, 0);

    /* Parameter and return value conversion */
    C99_CHECK_EQ(ret_bool_from_int(512), 1);
    C99_CHECK_EQ(ret_bool_from_int(0), 0);
    C99_CHECK_EQ(ret_bool_from_dbl(0.5), 1);
    C99_CHECK_EQ(ret_bool_from_dbl(neg_zero), 0);
    C99_CHECK_EQ(take_bool(256), 1);
    C99_CHECK_EQ(take_bool(0), 0);

    C99_END();
}
