/*
 * C17 Conformance Test — Class C (§6.8.4.2 / §6.7.2.1 / §6.5 / Annex F)
 * Adversarial regression for:
 *   1) 64-bit switch extremes: INT64_MAX (loop-wrap hazard), dense INT64_MIN..+3
 *      jump-table (signed -min_val overflow), sparse INT64_MIN..INT64_MAX span,
 *      and uint64_t boundary crossing 0x7fffffffffffffffULL / 0x8000000000000000ULL.
 *   2) Volatile bitfields and volatile struct member reads/writes (must retain
 *      [volatile] across -O2 GVN/DSE/LICM).
 *   3) IEEE-754 floating-point identities optimizers must NOT mis-fold:
 *      NaN != NaN, !(NaN == NaN), Inf - Inf -> NaN, (-0.0) + (-0.0) signbit,
 *      (-0.0) + (+0.0) -> +0.0, (-0.0) == (+0.0).
 */
#include "c17_test.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

static int sw_i64_max(int64_t x) {
    switch (x) {
        case INT64_MAX - 3: return 11;
        case INT64_MAX - 2: return 12;
        case INT64_MAX - 1: return 13;
        case INT64_MAX:     return 14;
        default:            return -1;
    }
}

static int sw_i64_min_dense(int64_t x) {
    switch (x) {
        case INT64_MIN:     return 21;
        case INT64_MIN + 1: return 22;
        case INT64_MIN + 2: return 23;
        case INT64_MIN + 3: return 24;
        default:            return -2;
    }
}

static int sw_i64_sparse_full_span(int64_t x) {
    switch (x) {
        case INT64_MIN: return 31;
        case -1:        return 32;
        case 0:         return 33;
        case INT64_MAX: return 34;
        default:        return -3;
    }
}

static int sw_u64_sign_boundary(uint64_t x) {
    switch (x) {
        case UINT64_C(0x7ffffffffffffffe): return 41;
        case UINT64_C(0x7fffffffffffffff):
        case UINT64_C(0x8000000000000000): return 42;
        case UINT64_C(0x8000000000000001): return 43;
        case UINT64_MAX - 1:               return 44;
        case UINT64_MAX:                   return 45;
        default:                           return -4;
    }
}

struct VolBits {
    volatile unsigned a : 5;
    volatile int      b : 6;
    volatile _Bool    c : 1;
    volatile unsigned d : 20;
};

static uint64_t f64_bits(double d) {
    uint64_t u = 0;
    memcpy(&u, &d, sizeof(u));
    return u;
}

static double fp_sub_self(double x) { return x - x; }
static int    fp_eq_self(double x)  { return x == x; }
static int    fp_ne_self(double x)  { return x != x; }
static int    fp_lt_self(double x)  { return x < x; }
static double fp_add_pair(double x, double y) { return x + y; }

int main(void) {
    C17_BEGIN("c68_bug_switch_64bit_extremes_and_volatile_bf",
              "64-bit switch extremes, volatile bitfields, IEEE-754 anti-mis-fold");

    /* 1. 64-bit switch extremes */
    volatile int64_t imax = INT64_MAX;
    volatile int64_t imin = INT64_MIN;
    C17_CHECK(sw_i64_max(imax)     == 14);
    C17_CHECK(sw_i64_max(imax - 1) == 13);
    C17_CHECK(sw_i64_max(imax - 3) == 11);
    C17_CHECK(sw_i64_max(0)        == -1);
    C17_CHECK(sw_i64_max(imin)     == -1);

    C17_CHECK(sw_i64_min_dense(imin)     == 21);
    C17_CHECK(sw_i64_min_dense(imin + 1) == 22);
    C17_CHECK(sw_i64_min_dense(imin + 2) == 23);
    C17_CHECK(sw_i64_min_dense(imin + 3) == 24);
    C17_CHECK(sw_i64_min_dense(imin + 4) == -2);
    C17_CHECK(sw_i64_min_dense(0)        == -2);
    C17_CHECK(sw_i64_min_dense(imax)     == -2);

    C17_CHECK(sw_i64_sparse_full_span(imin) == 31);
    C17_CHECK(sw_i64_sparse_full_span(-1)   == 32);
    C17_CHECK(sw_i64_sparse_full_span(0)    == 33);
    C17_CHECK(sw_i64_sparse_full_span(imax) == 34);
    C17_CHECK(sw_i64_sparse_full_span(1)    == -3);

    volatile uint64_t uhi = UINT64_C(0x8000000000000000);
    volatile uint64_t umax = UINT64_MAX;
    C17_CHECK(sw_u64_sign_boundary(uhi - 2)  == 41);
    C17_CHECK(sw_u64_sign_boundary(uhi - 1)  == 42);
    C17_CHECK(sw_u64_sign_boundary(uhi)      == 42);
    C17_CHECK(sw_u64_sign_boundary(uhi + 1)  == 43);
    C17_CHECK(sw_u64_sign_boundary(umax - 1) == 44);
    C17_CHECK(sw_u64_sign_boundary(umax)     == 45);
    C17_CHECK(sw_u64_sign_boundary(0)        == -4);

    /* 2. Volatile bitfields & volatile struct accesses */
    struct VolBits vb = {0};
    vb.a = 29u;
    vb.b = -19;
    vb.c = 1;
    vb.d = 0x54321u;
    C17_CHECK((unsigned)vb.a == 29u);
    C17_CHECK((int)vb.b      == -19);
    C17_CHECK((int)vb.c      == 1);
    C17_CHECK((unsigned)vb.d == 0x54321u);

    /* Redundant-looking overwrite to volatile bitfield must still take effect */
    vb.a = 3u;
    vb.a = 17u;
    C17_CHECK((unsigned)vb.a == 17u);
    C17_CHECK((int)vb.b      == -19);

    volatile struct VolBits vwhole = { .a = 11u, .b = -7, .c = 1, .d = 0x12345u };
    C17_CHECK((unsigned)vwhole.a == 11u);
    C17_CHECK((int)vwhole.b      == -7);
    C17_CHECK((unsigned)vwhole.d == 0x12345u);

    /* 3. IEEE-754 floating-point anti-mis-fold checks */
    volatile double z = 0.0;
    volatile double nz = -0.0;
    volatile double inf = 1.0 / z;
    volatile double nan_v = z / z;

    C17_CHECK(isnan(fp_sub_self(inf)));
    C17_CHECK(isnan(fp_sub_self(nan_v)));
    C17_CHECK(fp_eq_self(nan_v) == 0);
    C17_CHECK(fp_ne_self(nan_v) == 1);
    C17_CHECK(fp_lt_self(nan_v) == 0);
    C17_CHECK(fp_eq_self(inf)   == 1);
    C17_CHECK(fp_ne_self(inf)   == 0);

    C17_CHECK(nz == z);
    C17_CHECK(f64_bits(fp_add_pair(nz, nz)) == UINT64_C(0x8000000000000000));
    C17_CHECK(f64_bits(fp_add_pair(nz, z))  == UINT64_C(0x0000000000000000));

    C17_END();
}
