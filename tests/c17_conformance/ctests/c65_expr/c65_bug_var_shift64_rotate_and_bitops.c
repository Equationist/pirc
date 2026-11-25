/*
 * C17 Conformance Test — Class C (§6.5.7 / §6.5.10-12)
 * Adversarial regression for runtime variable 32/64-bit shifts (0..63),
 * guarded rotates, bit-reversal, popcount, and byte-swap chains across
 * -O0/-O1/-O2 (ARM64 lslv/lsrv/asrv/extr & x64 cl-register shifts).
 */
#include "c17_test.h"
#include <stdint.h>

static uint64_t rotl64(uint64_t x, unsigned s) {
    s &= 63u;
    return s == 0u ? x : ((x << s) | (x >> (64u - s)));
}

static uint32_t rotr32(uint32_t x, unsigned s) {
    s &= 31u;
    return s == 0u ? x : ((x >> s) | (x << (32u - s)));
}

static uint64_t bitrev64(uint64_t x) {
    uint64_t r = 0u;
    for (unsigned i = 0u; i < 64u; i++) {
        r = (r << 1u) | ((x >> i) & UINT64_C(1));
    }
    return r;
}

static unsigned popcnt64(uint64_t x) {
    unsigned c = 0u;
    while (x != 0u) {
        x &= (x - UINT64_C(1));
        c++;
    }
    return c;
}

static uint64_t bswap64_sw(uint64_t x) {
    return ((x & UINT64_C(0x00000000000000ff)) << 56) |
           ((x & UINT64_C(0x000000000000ff00)) << 40) |
           ((x & UINT64_C(0x0000000000ff0000)) << 24) |
           ((x & UINT64_C(0x00000000ff000000)) <<  8) |
           ((x & UINT64_C(0x000000ff00000000)) >>  8) |
           ((x & UINT64_C(0x0000ff0000000000)) >> 24) |
           ((x & UINT64_C(0x00ff000000000000)) >> 40) |
           ((x & UINT64_C(0xff00000000000000)) >> 56);
}

static uint64_t var_shl64(uint64_t x, unsigned s) { return x << s; }
static uint64_t var_shr64(uint64_t x, unsigned s) { return x >> s; }
static uint32_t var_shl32(uint32_t x, unsigned s) { return x << s; }
static uint32_t var_shr32(uint32_t x, unsigned s) { return x >> s; }

int main(void) {
    C17_BEGIN("c65_bug_var_shift64_rotate_and_bitops",
              "Variable 32/64-bit shifts 0..63, guarded rotates, bitrev, popcnt, bswap");

    uint64_t v = c17_opaque_ull(UINT64_C(0x0123456789abcdef));

    /* Variable shift edge counts: 0, 1, 31, 32, 63 */
    C17_CHECK(var_shl64(v, (unsigned)c17_opaque_int(0))  == v);
    C17_CHECK(var_shr64(v, (unsigned)c17_opaque_int(0))  == v);
    C17_CHECK(var_shl64(v, (unsigned)c17_opaque_int(32)) == UINT64_C(0x89abcdef00000000));
    C17_CHECK(var_shr64(v, (unsigned)c17_opaque_int(32)) == UINT64_C(0x0000000001234567));
    C17_CHECK(var_shl64(v, (unsigned)c17_opaque_int(63)) == UINT64_C(0x8000000000000000));
    C17_CHECK(var_shr64(UINT64_C(0x8000000000000000), (unsigned)c17_opaque_int(63)) == UINT64_C(1));

    uint32_t v32 = (uint32_t)c17_opaque_ull(UINT64_C(0x89abcdef));
    C17_CHECK(var_shl32(v32, (unsigned)c17_opaque_int(0))  == v32);
    C17_CHECK(var_shr32(v32, (unsigned)c17_opaque_int(0))  == v32);
    C17_CHECK(var_shl32(v32, (unsigned)c17_opaque_int(31)) == UINT32_C(0x80000000));
    C17_CHECK(var_shr32(v32, (unsigned)c17_opaque_int(31)) == UINT32_C(1));

    /* Full 0..64 rotate round-trip */
    for (unsigned s = 0u; s <= 64u; s++) {
        uint64_t r = rotl64(v, (unsigned)c17_opaque_int((int)s));
        uint64_t back = rotl64(r, (unsigned)c17_opaque_int((int)(64u - (s & 63u))));
        C17_CHECK(back == v);
    }

    /* Full 0..32 rotate round-trip */
    for (unsigned s = 0u; s <= 32u; s++) {
        uint32_t r = rotr32(v32, (unsigned)c17_opaque_int((int)s));
        uint32_t back = rotr32(r, (unsigned)c17_opaque_int((int)(32u - (s & 31u))));
        C17_CHECK(back == v32);
    }

    /* Bit-reverse involution & popcount & bswap */
    uint64_t rev = bitrev64(v);
    C17_CHECK(bitrev64(rev) == v);
    C17_CHECK(rev == UINT64_C(0xf7b3d591e6a2c480));
    C17_CHECK(popcnt64(v) == 32u);
    C17_CHECK(popcnt64(rev) == 32u);
    C17_CHECK(bswap64_sw(v) == UINT64_C(0xefcdab8967452301));
    C17_CHECK(bswap64_sw(bswap64_sw(v)) == v);

    C17_END();
}
