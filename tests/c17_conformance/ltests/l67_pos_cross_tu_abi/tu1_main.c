/* Cross-TU struct/HFA/bitfield/varargs ABI test (TU 1 - caller) */
#include "c17_test.h"
#include "abi_common.h"

int main(void) {
    C99_BEGIN("l67_pos_cross_tu_abi", "Cross-TU struct, HFA, bitfield, and varargs ABI");

    struct Odd3 o = xtu_odd3((struct Odd3){ 1, 2, 3 });
    C99_CHECK_EQ(o.a, 2);
    C99_CHECK_EQ(o.b, 4);
    C99_CHECK_EQ(o.c, 6);

    struct Big24 b = xtu_big24((struct Big24){ 10, 20, 30 }, (struct Big24){ 1, 2, 3 });
    C99_CHECK_EQ(b.x, 11);
    C99_CHECK_EQ(b.y, 22);
    C99_CHECK_EQ(b.z, 33);

    struct Hfa4 h = xtu_hfa4((struct Hfa4){ 1.0, 2.0, 3.0, 4.0 },
                             (struct Hfa4){ 0.5, 0.5, 0.5, 0.5 });
    C99_CHECK_DBL(h.a + h.b + h.c + h.d, 12.0, 0.0);

    struct BfPair bf = xtu_bf((struct BfPair){ 12u, -9 });
    C99_CHECK_EQ(bf.lo, 13u);
    C99_CHECK_EQ(bf.hi, -8);

    double vs = xtu_varargs(5, 1.5, 2.5, 3.5, 4.5, 5.5);
    C99_CHECK_DBL(vs, 17.5, 0.0);

    C99_END();
}
