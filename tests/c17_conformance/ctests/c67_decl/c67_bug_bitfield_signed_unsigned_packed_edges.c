/*
 * C17 Conformance Test — Class C (§6.7.2.1 / §6.5.16.2)
 * Exhaustive bitfield width & boundary stress across widths 1, 2, 3, 7, 9,
 * 15, 17, 24, 31, 32, 33, 48, and 63 bits (signed & unsigned), crossing byte,
 * 32-bit, and 64-bit boundaries, plus compound assignments & inc/dec.
 */
#include "c17_test.h"
#include <stdint.h>

struct WideBitfields {
    unsigned u1  : 1;
    signed   s2  : 2;
    unsigned u3  : 3;
    signed   s7  : 7;
    unsigned u9  : 9;
    signed   s15 : 15;
    unsigned u17 : 17;
    signed   s24 : 24;
    unsigned u31 : 31;
    unsigned u32 : 32;
    uint64_t u33 : 33;
    int64_t  s48 : 48;
    uint64_t u63 : 63;
};

int main(void) {
    C17_BEGIN("c67_bug_bitfield_signed_unsigned_packed_edges",
              "Bitfields 1..63 bits signed/unsigned, cross-word RMW, compound assign");

    struct WideBitfields w = {0};
    w.u1  = 1u;
    w.s2  = -2;
    w.u3  = 5u;
    w.s7  = -63;
    w.u9  = 345u;
    w.s15 = -12345;
    w.u17 = 100001u;
    w.s24 = -7000000;
    w.u31 = 0x7abcde12u;
    w.u32 = 0xdeadbeefu;
    w.u33 = UINT64_C(0x1abcdef12);
    w.s48 = -INT64_C(0x555512345678);
    w.u63 = UINT64_C(0x7fedcba987654321);

    C17_CHECK(w.u1  == 1u);
    C17_CHECK(w.s2  == -2);
    C17_CHECK(w.u3  == 5u);
    C17_CHECK(w.s7  == -63);
    C17_CHECK(w.u9  == 345u);
    C17_CHECK(w.s15 == -12345);
    C17_CHECK(w.u17 == 100001u);
    C17_CHECK(w.s24 == -7000000);
    C17_CHECK(w.u31 == 0x7abcde12u);
    C17_CHECK(w.u32 == 0xdeadbeefu);
    C17_CHECK(w.u33 == UINT64_C(0x1abcdef12));
    C17_CHECK(w.s48 == -INT64_C(0x555512345678));
    C17_CHECK(w.u63 == UINT64_C(0x7fedcba987654321));

    /* Compound assignments & wraparound must NOT corrupt neighboring fields */
    w.u3 += 5u;   /* (5 + 5) & 7 == 2 */
    w.s7 += 10;   /* -53 */
    w.u9++;       /* 346 */
    --w.s15;      /* -12346 */
    w.u17 ^= 0x1ffffu;
    w.u33 += UINT64_C(0x100000000); /* wraps mod 2^33 */
    w.s48 += INT64_C(0x10000);

    C17_CHECK(w.u1  == 1u);
    C17_CHECK(w.s2  == -2);
    C17_CHECK(w.u3  == 2u);
    C17_CHECK(w.s7  == -53);
    C17_CHECK(w.u9  == 346u);
    C17_CHECK(w.s15 == -12346);
    C17_CHECK(w.u17 == (100001u ^ 0x1ffffu));
    C17_CHECK(w.s24 == -7000000);
    C17_CHECK(w.u31 == 0x7abcde12u);
    C17_CHECK(w.u32 == 0xdeadbeefu);
    C17_CHECK(w.u33 == UINT64_C(0x0abcdef12));
    C17_CHECK(w.s48 == (-INT64_C(0x555512345678) + INT64_C(0x10000)));
    C17_CHECK(w.u63 == UINT64_C(0x7fedcba987654321));

    C17_END();
}
