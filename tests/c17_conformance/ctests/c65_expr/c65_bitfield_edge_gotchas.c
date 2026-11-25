/*
 * C17 §6.7.2.1 & §6.5.16.1: Bitfield torture — many unaligned bit widths
 * spanning multiple words without neighbor corruption, pre/post inc/dec wrap
 * return values, dynamic arrays of bitfield structs, and chained assignment
 * truncation (a = b.u4 = 100 -> a == 4!).
 */
#include "c17_test.h"

struct MultiWordBF {
    unsigned int u1  : 1;
    signed int   s1  : 1;
    unsigned int u3  : 3;
    signed int   s4  : 4;
    unsigned int u7  : 7;
    signed int   s9  : 9;
    unsigned int u11 : 11;
    signed int   s15 : 15;
    unsigned int u19 : 19;
    signed int   s23 : 23;
    unsigned int u31 : 31;
    unsigned int u32 : 32;
};

int main(void) {
    C17_BEGIN("c65_bitfield_edge_gotchas", "Multi-word bitfields, inc/dec wrap, arrays, chained assignment");

    struct MultiWordBF m = { 0 };
    m.u1  = 1u;
    m.s1  = -1;
    m.u3  = 6u;
    m.s4  = -7;
    m.u7  = 111u;
    m.s9  = -200;
    m.u11 = 1999u;
    m.s15 = -12345;
    m.u19 = 400000u;
    m.s23 = -3000000;
    m.u31 = 2000000000u;
    m.u32 = 0xDEADBEEFu;

    /* Overwrite middle fields and verify ALL neighbors remain intact */
    m.s9 = 175;
    m.u19 = 123456u;

    C17_CHECK_EQ(m.u1, 1u);
    C17_CHECK_EQ(m.s1, -1);
    C17_CHECK_EQ(m.u3, 6u);
    C17_CHECK_EQ(m.s4, -7);
    C17_CHECK_EQ(m.u7, 111u);
    C17_CHECK_EQ(m.s9, 175);
    C17_CHECK_EQ(m.u11, 1999u);
    C17_CHECK_EQ(m.s15, -12345);
    C17_CHECK_EQ(m.u19, 123456u);
    C17_CHECK_EQ(m.s23, -3000000);
    C17_CHECK_EQ(m.u31, 2000000000u);
    C17_CHECK_UEQ(m.u32, 0xDEADBEEFu);

    /* Pre/post increment wrap on 3-bit unsigned bitfield */
    m.u3 = 7u;
    C17_CHECK_EQ(m.u3++, 7u);
    C17_CHECK_EQ(m.u3, 0u);
    C17_CHECK_EQ(--m.u3, 7u);
    C17_CHECK_EQ(++m.u3, 0u);

    /* Chained assignment: value of (b.u3 = 13) is truncated to 3 bits (5)! */
    int chain_res = (m.u3 = (unsigned int)c17_opaque_int(13));
    C17_CHECK_EQ(m.u3, 5u);
    C17_CHECK_EQ(chain_res, 5);

    /* Dynamic array of bitfield structs */
    struct MultiWordBF arr[4] = { { 0 } };
    for (int i = 0; i < 4; i++) {
        int idx = c17_opaque_int(i);
        arr[idx].u7 = (unsigned int)(idx * 17 + 3);
        arr[idx].s15 = -100 * idx - 5;
    }
    C17_CHECK_EQ(arr[2].u7, 37u);
    C17_CHECK_EQ(arr[3].s15, -305);

    C17_END();
}
