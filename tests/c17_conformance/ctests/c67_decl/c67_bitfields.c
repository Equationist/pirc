/*
 * C99 §6.7.2.1: Bitfields — signed, unsigned, _Bool, zero-width alignment,
 * unnamed bitfields, cross-byte packing, global bitfield initializers,
 * unions with bitfields, compound ops, and pass/return by value.
 */
#include "c17_test.h"

struct PackedBits {
    unsigned int a : 5;   /* bits 0..4 */
    signed int   b : 7;   /* bits 5..11 (crosses byte 0->1!) */
    unsigned int c : 9;   /* bits 12..20 (crosses byte 1->2!) */
    _Bool        d : 1;   /* bit 21 */
    unsigned int   : 3;   /* unnamed padding */
    signed int   e : 7;   /* bits 25..31 */
};

struct ZeroWidthBits {
    unsigned int x : 3;
    unsigned int   : 0;   /* force next allocation unit */
    unsigned int y : 3;
};

union BitUnion {
    struct {
        unsigned int lo : 4;
        unsigned int hi : 4;
    } nib;
    unsigned char raw;
};

/* Global static bitfield initializer */
static struct PackedBits g_bits = {
    .a = 29u,
    .b = -41,
    .c = 405u,
    .d = 1,
    .e = -63,
};

static struct PackedBits transform_bits(struct PackedBits in) {
    struct PackedBits out = in;
    out.a += 1u;
    out.b -= 2;
    out.c ^= 0x155u;
    out.d = 5; /* _Bool bitfield: nonzero MUST normalize to 1! */
    out.e += 10;
    return out;
}

int main(void) {
    C99_BEGIN("c67_bitfields", "Bitfields: signed/unsigned/_Bool, cross-byte, 0-width, global, ABI");

    /* Verify global bitfield initializer */
    C99_CHECK_EQ(g_bits.a, 29u);
    C99_CHECK_EQ(g_bits.b, -41);
    C99_CHECK_EQ(g_bits.c, 405u);
    C99_CHECK_EQ(g_bits.d, 1);
    C99_CHECK_EQ(g_bits.e, -63);

    /* Pass and return by value + mutations */
    struct PackedBits r = transform_bits(g_bits);
    C99_CHECK_EQ(r.a, 30u);
    C99_CHECK_EQ(r.b, -43);
    C99_CHECK_EQ(r.c, 405u ^ 0x155u);
    C99_CHECK_EQ(r.d, 1);
    C99_CHECK_EQ(r.e, -53);

    /* _Bool bitfield normalization from 256 (low bit 0!) */
    r.d = c99_opaque_int(256);
    C99_CHECK_EQ(r.d, 1);
    r.d = c99_opaque_int(0);
    C99_CHECK_EQ(r.d, 0);

    /* Unsigned wraparound in narrow bitfield */
    r.a = 31u;
    C99_CHECK_EQ(r.a++, 31u);
    C99_CHECK_EQ(r.a, 0u);
    C99_CHECK_EQ(--r.a, 31u);

    /* Signed 1-bit bitfield holds 0 and -1 */
    struct { signed int s1 : 1; } onebit = { 0 };
    onebit.s1 = -1;
    C99_CHECK_EQ(onebit.s1, -1);
    onebit.s1 = 0;
    C99_CHECK_EQ(onebit.s1, 0);

    /* Zero-width bitfield forces separate storage unit */
    C99_CHECK(sizeof(struct ZeroWidthBits) >= 2 * sizeof(int));
    struct ZeroWidthBits zw = { 5u, 6u };
    C99_CHECK_EQ(zw.x, 5u);
    C99_CHECK_EQ(zw.y, 6u);

    /* Bitfield inside union */
    union BitUnion bu;
    bu.raw = 0;
    bu.nib.lo = 0xAu;
    bu.nib.hi = 0x5u;
    C99_CHECK_EQ(bu.nib.lo, 0xAu);
    C99_CHECK_EQ(bu.nib.hi, 0x5u);

    C99_END();
}
