/*
 * C99 §6.3.1.1 & §6.3.1.8: Integer promotions, usual arithmetic conversions,
 * bitfield promotion rules, and unsigned wraparound.
 */
#include "c17_test.h"
#include <limits.h>
#include <stdint.h>

struct BF {
    unsigned int u5 : 5;
    unsigned int u32 : 32;
    signed int s5 : 5;
};

int main(void) {
    C99_BEGIN("c63_int_promotions", "Integer promotions, usual arith conv, bitfield promotions");

    /* Small types promote to int under unary + and arithmetic */
    signed char sc = -5;
    unsigned char uc = 200;
    short sh = -10;
    unsigned short ush = 60000;
    C99_CHECK_EQ(sizeof(+sc), sizeof(int));
    C99_CHECK_EQ(sizeof(+uc), sizeof(int));
    C99_CHECK_EQ(sizeof(+sh), sizeof(int));
    C99_CHECK_EQ(sizeof(+ush), sizeof(int));

    /* unsigned char + signed char -> both promote to int, NOT unsigned! */
    C99_CHECK((-1 < uc));
    C99_CHECK((sc - uc) < 0);
    C99_CHECK_EQ(sc - uc, -205);

    /* unsigned short promotes to int when INT_MAX >= USHRT_MAX */
    C99_CHECK((ush - 65000) < 0);
    C99_CHECK_EQ(ush - 65000, -5000);

    /* int vs unsigned int -> unsigned int */
    int neg1 = c99_opaque_int(-1);
    unsigned int u1 = 1u;
    C99_CHECK((neg1 < u1) == 0); /* -1 converts to UINT_MAX > 1u */

    /* §6.3.1.8: unsigned int vs signed long long -> signed long long (can represent all uint) */
    long long sll = -1LL;
    C99_CHECK((sll < u1) == 1);
    C99_CHECK((sll + u1) == 0LL);

    /* On LP64 (sizeof(long) > sizeof(unsigned int)), unsigned int vs signed long -> signed long */
    if (sizeof(long) > sizeof(unsigned int)) {
        long sl = -1L;
        C99_CHECK((sl < u1) == 1);
        C99_CHECK((sl + u1) == 0L);
    }

    /* §6.3.1.1 p2: bitfield promotion!
     * unsigned int u5:5 fits in int -> promotes to SIGNED int!
     * unsigned int u32:32 does NOT fit in int -> stays UNSIGNED int! */
    struct BF bf = { 5u, 5u, -3 };
    C99_CHECK((-bf.u5) < 0);
    C99_CHECK_EQ(-bf.u5, -5);
    C99_CHECK((-bf.u32) > 0);
    C99_CHECK_UEQ(-bf.u32, (unsigned int)-5);
    C99_CHECK_EQ(bf.s5, -3);
    C99_CHECK((bf.s5 + 1) == -2);

    /* Unsigned wraparound modulo 2^N */
    unsigned int zero = (unsigned int)c99_opaque_int(0);
    C99_CHECK_UEQ(zero - 1u, UINT_MAX);
    unsigned long long z64 = c99_opaque_ull(0ULL);
    C99_CHECK_UEQ(z64 - 1ULL, ULLONG_MAX);

    /* Shift promotion: unsigned char promotes to int before shifting */
    unsigned char uc_ff = (unsigned char)c99_opaque_int(0xff);
    C99_CHECK_EQ(uc_ff << 1, 510);
    C99_CHECK_EQ(sizeof(uc_ff << 1), sizeof(int));

    C99_END();
}
