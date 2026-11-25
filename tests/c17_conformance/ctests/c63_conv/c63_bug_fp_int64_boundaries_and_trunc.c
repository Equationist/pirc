/*
 * C17 Conformance Test — Class C (§6.3.1.4 / Annex F)
 * Adversarial regression for float/double <-> signed/unsigned 32/64-bit
 * integer boundary conversions (GCC / LLVM / QBE / TCC / chibicc hotbed):
 *   - 2^53, 2^63, 2^63 - 1024, UINT64_MAX -> double -> uint64_t / int64_t
 *   - -2^63 (INT64_MIN), INT32_MIN, UINT32_MAX, 2^31
 *   - Truncation toward zero for positive and negative fractional values
 *     (+0.999999 -> 0, -0.999999 -> 0, -1.999999 -> -1, +1.999999 -> 1)
 *   - Exact float <-> int32_t / uint32_t / int64_t / uint64_t powers of two.
 */
#include "c17_test.h"
#include <limits.h>
#include <stdint.h>

static int64_t  d2i64(double d)   { return (int64_t)d; }
static uint64_t d2u64(double d)   { return (uint64_t)d; }
static int32_t  d2i32(double d)   { return (int32_t)d; }
static uint32_t d2u32(double d)   { return (uint32_t)d; }
static int64_t  f2i64(float f)    { return (int64_t)f; }
static uint64_t f2u64(float f)    { return (uint64_t)f; }
static double   i642d(int64_t x)  { return (double)x; }
static double   u642d(uint64_t x) { return (double)x; }
static float    i642f(int64_t x)  { return (float)x; }
static float    u642f(uint64_t x) { return (float)x; }

int main(void) {
    C17_BEGIN("c63_bug_fp_int64_boundaries_and_trunc",
              "IEEE-754 float/double <-> i32/u32/i64/u64 boundary conversions");

    /* Truncation toward zero */
    C17_CHECK(d2i64(c17_opaque_dbl(0.999999999))  == 0);
    C17_CHECK(d2i64(c17_opaque_dbl(-0.999999999)) == 0);
    C17_CHECK(d2i64(c17_opaque_dbl(1.999999999))  == 1);
    C17_CHECK(d2i64(c17_opaque_dbl(-1.999999999)) == -1);
    C17_CHECK(d2u64(c17_opaque_dbl(0.999999999))  == UINT64_C(0));
    C17_CHECK(d2i32(c17_opaque_dbl(-0.75))        == 0);
    C17_CHECK(d2u32(c17_opaque_dbl(4294967295.0)) == UINT32_MAX);
    C17_CHECK(d2i32(c17_opaque_dbl(-2147483648.0)) == INT32_MIN);
    C17_CHECK(d2i32(c17_opaque_dbl(2147483647.0))  == INT32_MAX);

    /* Exact 2^53 and 2^63 boundaries */
    uint64_t p53 = UINT64_C(1) << 53;
    C17_CHECK(d2u64(u642d(c17_opaque_ull(p53))) == p53);
    C17_CHECK(d2u64(u642d(c17_opaque_ull(p53 - 1u))) == p53 - 1u);
    C17_CHECK(d2i64(i642d(c17_opaque_ll(-(int64_t)p53))) == -(int64_t)p53);

    /* >= 2^63 unsigned conversions (classic x86/ARM64/QBE bug!) */
    uint64_t p63 = UINT64_C(1) << 63;
    double dp63 = u642d(c17_opaque_ull(p63));
    C17_CHECK(dp63 == 9223372036854775808.0);
    C17_CHECK(d2u64(dp63) == p63);

    uint64_t hi_u64 = p63 + (UINT64_C(1) << 52) + (UINT64_C(1) << 20);
    double d_hi = u642d(c17_opaque_ull(hi_u64));
    C17_CHECK(d2u64(d_hi) == hi_u64);

    uint64_t near_max = UINT64_C(0xfffffffffffff800); /* largest exact double < 2^64 */
    C17_CHECK(d2u64(u642d(c17_opaque_ull(near_max))) == near_max);

    /* INT64_MIN exact round-trip */
    double dmin = i642d(c17_opaque_ll(INT64_MIN));
    C17_CHECK(dmin == -9223372036854775808.0);
    C17_CHECK(d2i64(dmin) == INT64_MIN);

    /* float <-> 64-bit integer boundaries */
    float fp63 = u642f(c17_opaque_ull(p63));
    C17_CHECK(f2u64(fp63) == p63);
    float fmin = i642f(c17_opaque_ll(INT64_MIN));
    C17_CHECK(f2i64(fmin) == INT64_MIN);
    uint64_t f_exact = p63 + (UINT64_C(1) << 45);
    C17_CHECK(f2u64(u642f(c17_opaque_ull(f_exact))) == f_exact);

    /* UINT64_MAX -> double rounds up to 2^64 = 18446744073709551616.0 */
    C17_CHECK(u642d(c17_opaque_ull(UINT64_MAX)) == 18446744073709551616.0);

    C17_END();
}
