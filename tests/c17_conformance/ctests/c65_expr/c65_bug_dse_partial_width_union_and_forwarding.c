/*
 * C17 Conformance Test — Class C (§6.5 / §6.5.2.3)
 * Adversarial optimizer regression for DSE, store-to-load forwarding, and GVN:
 *   1) Overlapping partial-width stores (i64 -> i32 -> i16 -> i8 -> i64):
 *      DSE must NOT drop a wider store when overwritten by a narrower store,
 *      and must NOT forward a stale wider store past a narrower partial write.
 *   2) Same-block union type-punning (uint64_t <-> double, uint32_t <-> float):
 *      forwarding must respect type equality and preserve bit patterns.
 *   3) Diamond-CFG conditional partial clobber preventing invalid load reuse.
 *   4) Overlapping memmove vs small-struct memcpy expansion.
 */
#include "c17_test.h"
#include <stdint.h>
#include <string.h>

union Pun64 {
    uint64_t u64;
    double   f64;
    uint32_t u32[2];
    uint16_t u16[4];
    uint8_t  u8[8];
};

union Pun32 {
    uint32_t u32;
    float    f32;
    uint16_t u16[2];
    uint8_t  u8[4];
};

static uint64_t partial_overwrite_seq(uint64_t init, uint32_t lo32, uint16_t mid16, uint8_t b7) {
    union Pun64 p;
    p.u64 = init;
    /* Partial overwrite low 32 bits; upper 32 bits of init MUST survive */
    p.u32[0] = lo32;
    /* Partial overwrite bytes [2..3]; bytes [0..1] and [4..7] MUST survive */
    p.u16[1] = mid16;
    /* Partial overwrite byte 7; bytes [0..6] MUST survive */
    p.u8[7] = b7;
    return p.u64;
}

static uint64_t dead_store_after_narrow_read(uint64_t a, uint64_t b, uint32_t *out_hi) {
    union Pun64 p;
    p.u64 = a;
    *out_hi = p.u32[1]; /* Reads high 32 bits of a: p.u64 = a is OBSERVED! */
    p.u64 = b;          /* Overwrites all 64 bits */
    return p.u64;
}

static double pun_u64_to_f64_same_block(uint64_t bits) {
    union Pun64 p;
    p.u64 = bits;
    return p.f64;
}

static uint64_t pun_f64_to_u64_same_block(double val) {
    union Pun64 p;
    p.f64 = val;
    return p.u64;
}

static float pun_u32_to_f32_same_block(uint32_t bits) {
    union Pun32 p;
    p.u32 = bits;
    return p.f32;
}

static uint32_t pun_f32_to_u32_same_block(float val) {
    union Pun32 p;
    p.f32 = val;
    return p.u32;
}

static uint64_t diamond_partial_clobber(uint64_t init, int flag, uint8_t patch) {
    union Pun64 p;
    p.u64 = init;
    uint64_t before = p.u64;
    if (flag) {
        p.u8[3] = patch;
    }
    return before ^ p.u64;
}

struct Small15 {
    uint8_t b[15];
};

static struct Small15 copy_small15(struct Small15 in) {
    struct Small15 tmp = in;
    struct Small15 out = tmp;
    return out;
}

int main(void) {
    C17_BEGIN("c65_bug_dse_partial_width_union_and_forwarding",
              "DSE partial-width stores, union punning forwarding, diamond clobber");

    /* 1. Partial overwrites (little-endian ARM64/x64) */
    uint64_t r1 = partial_overwrite_seq(
        c17_opaque_ull(UINT64_C(0x1122334455667788)),
        (uint32_t)c17_opaque_ull(UINT64_C(0xaabbccdd)),
        (uint16_t)c17_opaque_int(0xeeff),
        (uint8_t)c17_opaque_int(0x99));
    C17_CHECK(r1 == UINT64_C(0x99223344eeffccdd));

    /* 2. Store observed by narrower sub-offset read before full overwrite */
    uint32_t hi = 0;
    uint64_t r2 = dead_store_after_narrow_read(
        c17_opaque_ull(UINT64_C(0xdeadbeef12345678)),
        c17_opaque_ull(UINT64_C(0x0102030405060708)),
        &hi);
    C17_CHECK(hi == UINT32_C(0xdeadbeef));
    C17_CHECK(r2 == UINT64_C(0x0102030405060708));

    /* 3. Same-block union punning */
    double pi = c17_opaque_dbl(3.14159265358979323846);
    uint64_t pi_bits = pun_f64_to_u64_same_block(pi);
    C17_CHECK(pi_bits == UINT64_C(0x400921fb54442d18));
    C17_CHECK(pun_u64_to_f64_same_block(pi_bits) == pi);

    float fl = c17_opaque_flt(-2.5f);
    uint32_t fl_bits = pun_f32_to_u32_same_block(fl);
    C17_CHECK(fl_bits == UINT32_C(0xc0200000));
    C17_CHECK(pun_u32_to_f32_same_block(fl_bits) == fl);

    /* 4. Diamond partial clobber */
    uint64_t d0 = diamond_partial_clobber(
        c17_opaque_ull(UINT64_C(0x0123456789abcdef)), c17_opaque_int(0), 0xffu);
    uint64_t d1 = diamond_partial_clobber(
        c17_opaque_ull(UINT64_C(0x0123456789abcdef)), c17_opaque_int(1), 0x00u);
    C17_CHECK(d0 == UINT64_C(0));
    C17_CHECK(d1 == UINT64_C(0x0000000089000000));

    /* 5. Odd-size 15B struct copy + overlapping memmove */
    struct Small15 s15;
    for (int i = 0; i < 15; i++) s15.b[i] = (uint8_t)(i * 7 + 3);
    struct Small15 s15_out = copy_small15(s15);
    for (int i = 0; i < 15; i++)
        C17_CHECK(s15_out.b[i] == (uint8_t)(i * 7 + 3));

    uint8_t buf[24];
    for (int i = 0; i < 24; i++) buf[i] = (uint8_t)(i + 1);
    memmove(buf + 3, buf, 16);
    C17_CHECK(buf[3] == 1 && buf[4] == 2 && buf[18] == 16 && buf[19] == 20);

    C17_END();
}
