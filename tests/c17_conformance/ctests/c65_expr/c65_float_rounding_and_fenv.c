/*
 * C17 §5.2.4.2.2 & Annex F: IEEE 754 precision boundaries, FP non-associativity
 * (verifying optimizer does NOT illegally reassociate FP math!), and algebraic
 * identity traps (Inf - Inf = NaN, -1.0 * 0.0 = -0.0, -0.0 + 0.0 = +0.0).
 */
#include "c17_test.h"
#include <stdint.h>

static uint64_t dbl_bits(double d) {
    uint64_t u = 0;
    memcpy(&u, &d, sizeof(u));
    return u;
}

int main(void) {
    C17_BEGIN("c65_float_rounding_and_fenv", "IEEE precision limits, FP non-associativity, algebraic traps");

    /* Float32 precision boundary 2^24 = 16777216: +1 rounds to even -> 16777216 */
    float f24 = c17_opaque_flt(16777216.0f);
    C17_CHECK(f24 + 1.0f == f24);
    C17_CHECK(f24 + 2.0f == 16777218.0f);

    /* Double precision boundary 2^53 = 9007199254740992: +1 rounds to even */
    double d53 = c17_opaque_dbl(9007199254740992.0);
    C17_CHECK(d53 + 1.0 == d53);
    C17_CHECK(d53 + 2.0 == 9007199254740994.0);

    /* FP non-associativity: optimizer MUST NOT reassociate (a + b) + c into a + (b + c)! */
    double huge = c17_opaque_dbl(1e20);
    double nhuge = c17_opaque_dbl(-1e20);
    double one = c17_opaque_dbl(1.0);
    double assoc_left = (huge + nhuge) + one;  /* 0.0 + 1.0 = 1.0 */
    double assoc_right = huge + (nhuge + one); /* 1e20 + (-1e20) = 0.0 */
    C17_CHECK_DBL(assoc_left, 1.0, 0.0);
    C17_CHECK_DBL(assoc_right, 0.0, 0.0);

    /* Algebraic identity traps that naive GVN/const-fold gets wrong: */
    double inf = one / c17_opaque_dbl(0.0);
    double inf_sub = inf - inf; /* MUST be NaN, NOT 0.0! */
    C17_CHECK(inf_sub != inf_sub);

    double inf_mul0 = inf * c17_opaque_dbl(0.0); /* MUST be NaN, NOT 0.0! */
    C17_CHECK(inf_mul0 != inf_mul0);

    double neg_mul0 = c17_opaque_dbl(-1.0) * c17_opaque_dbl(0.0); /* MUST be -0.0! */
    C17_CHECK_UEQ(dbl_bits(neg_mul0), 0x8000000000000000ULL);

    double nz = -c17_opaque_dbl(0.0);
    double nz_plus0 = nz + c17_opaque_dbl(0.0); /* MUST be +0.0, NOT nz (-0.0)! */
    C17_CHECK_UEQ(dbl_bits(nz_plus0), 0x0000000000000000ULL);

    /* Ternary preserving -0.0 sign bit */
    double sel = c17_opaque_int(1) ? nz : c17_opaque_dbl(0.0);
    C17_CHECK_UEQ(dbl_bits(sel), 0x8000000000000000ULL);

    C17_END();
}
