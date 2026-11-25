/*
 * C17 Conformance Test — Class C (§6.3.1.1 / §6.5.2.5 / §6.7.2.1)
 * Adversarial regressions for:
 *   1) Narrow integer & _Bool unary promotions: ~(uint8_t)0 == -1,
 *      -(uint8_t)1 == -1, -(uint16_t)1 == -1, _Bool unary - / ~ / !,
 *      (int16_t)-32768 << 1? Wait: avoid left-shift of negative (UB in C17)!
 *      Test (uint16_t)0x8000 << 1 == 0x10000 (int promotion!),
 *      (uint8_t)0x80 >> 1 == 0x40, (int8_t)-2 / 2 == -1.
 *   2) Signed 1-bit bitfield (values 0 and -1), zero-width :0 bitfield
 *      alignment boundary, and bitfields inside a union.
 *   3) Compound literal re-initialization on each loop/goto execution
 *      (§6.5.2.5p6).
 */
#include "c17_test.h"
#include <stddef.h>
#include <stdint.h>

struct SignedOneBit {
    signed int b1 : 1;
    unsigned int u1 : 1;
    signed int b3 : 3;
};

struct ZeroWidthBf {
    unsigned int a : 5;
    unsigned int   : 0;
    unsigned int b : 5;
};

union BfUnion {
    struct {
        unsigned int lo : 4;
        unsigned int hi : 4;
    } bits;
    uint8_t raw;
};

static int compound_lit_loop_reinit(int n) {
    int sum = 0;
    int i = 0;
    int *prev_ptr = NULL;
    int same_addr_count = 0;
again:
    if (i < n) {
        int *p = (int[]){ 100, 200, 300 };
        /* Each execution of the compound literal re-initializes p[0]..p[2]! */
        sum += p[0] + p[1] + p[2];
        p[0] = -999;
        p[1] = -888;
        p[2] = -777;
        if (prev_ptr == p) same_addr_count++;
        prev_ptr = p;
        i++;
        goto again;
    }
    return sum + (same_addr_count >= 0 ? 0 : -1);
}

int main(void) {
    C17_BEGIN("c65_bug_narrow_promo_shifts_and_bitfield_zero",
              "Narrow/_Bool promotions, 1-bit signed & :0 bitfields, compound lit reinit");

    /* 1. Narrow & _Bool promotions */
    volatile uint8_t u8_0 = 0u, u8_1 = 1u, u8_80 = 0x80u;
    volatile uint16_t u16_1 = 1u, u16_hi = 0x8000u;
    volatile int8_t s8_m2 = -2;
    volatile _Bool b1 = 1, b0 = 0;

    C17_CHECK((~u8_0) == -1);
    C17_CHECK((-u8_1) == -1);
    C17_CHECK((-u16_1) == -1);
    C17_CHECK((u16_hi << 1) == 0x10000);
    C17_CHECK((u8_80 >> 1) == 0x40);
    C17_CHECK((s8_m2 / 2) == -1);
    C17_CHECK((-b1) == -1);
    C17_CHECK((~b1) == -2);
    C17_CHECK((!b1) == 0 && (!b0) == 1);
    C17_CHECK(_Generic(-u8_1, int: 1, unsigned: 2, default: 3) == 1);
    C17_CHECK(_Generic(~b1,   int: 1, _Bool: 2, default: 3) == 1);

    /* 2. Signed 1-bit bitfield, :0 alignment, union bitfield */
    struct SignedOneBit sob = { 0, 0, 0 };
    sob.b1 = 1;
    sob.u1 = 1;
    sob.b3 = -4;
    C17_CHECK(sob.b1 == -1);
    C17_CHECK(sob.u1 == 1u);
    C17_CHECK(sob.b3 == -4);
    sob.b1 = 0;
    C17_CHECK(sob.b1 == 0);

    struct ZeroWidthBf zw = { 0 };
    zw.a = 19u;
    zw.b = 27u;
    C17_CHECK(sizeof(struct ZeroWidthBf) >= 2 * sizeof(unsigned int));
    C17_CHECK(zw.a == 19u && zw.b == 27u);

    union BfUnion u;
    u.raw = 0xabu;
    C17_CHECK(u.bits.lo == 0xbu && u.bits.hi == 0xau);
    u.bits.hi = 0x5u;
    C17_CHECK(u.raw == 0x5bu);

    /* 3. Compound literal loop re-initialization */
    C17_CHECK(compound_lit_loop_reinit(c17_opaque_int(5)) == 5 * 600);

    C17_END();
}
