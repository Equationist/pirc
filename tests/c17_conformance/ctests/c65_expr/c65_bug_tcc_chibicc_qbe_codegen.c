/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.3 / §6.5 — Real-world compiler bug regressions:
 *   - Classic TCC, chibicc, cproc, and QBE codegen bugs:
 *     * unsigned short / unsigned char integer promotion in subtraction
 *     * unary minus on uint32_t zero-extended (NOT sign-extended!) to uint64_t
 *     * signed division/modulo edge cases (truncation toward zero)
 *     * double/float <-> uint64_t above 2^63
 *     * C11/C17 non-lvalue struct array member subscripting f().arr[i]
 *     * *p++->field pointer increment ordering
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

struct arr_box {
    int arr[4];
};

static struct arr_box make_arr_box(int base) {
    struct arr_box b;
    b.arr[0] = base;
    b.arr[1] = base + 10;
    b.arr[2] = base + 20;
    b.arr[3] = base + 30;
    return b;
}

struct node {
    int *ptr;
    int tag;
};

static uint64_t dbl_to_u64(double d) { return (uint64_t)d; }
static uint64_t flt_to_u64(float f) { return (uint64_t)f; }
static double u64_to_dbl(uint64_t u) { return (double)u; }
static float u64_to_flt(uint64_t u) { return (float)u; }

static int sdiv32(int a, int b) { return a / b; }
static int smod32(int a, int b) { return a % b; }
static long long sdiv64(long long a, long long b) { return a / b; }
static long long smod64(long long a, long long b) { return a % b; }

int main(void) {
    C17_BEGIN("c65_bug_tcc_chibicc_qbe_codegen",
              "TCC/chibicc/QBE promotions, uint64<->fp, div/mod, rvalue array");

    /* Unsigned short & unsigned char promote to signed int (when int > 16 bits) */
    {
        unsigned short a = (unsigned short)c17_opaque_int(1);
        unsigned short b = (unsigned short)c17_opaque_int(2);
        C17_CHECK((a - b) < 0);
        C17_CHECK_EQ(a - b, -1);

        unsigned char c1 = (unsigned char)c17_opaque_int(5);
        unsigned char c2 = (unsigned char)c17_opaque_int(10);
        C17_CHECK((c1 - c2) < 0);
        C17_CHECK_EQ(c1 - c2, -5);
    }

    /* Unary minus on uint32_t -> widened to uint64_t MUST zero-extend! */
    {
        uint32_t one = (uint32_t)c17_opaque_int(1);
        uint64_t wide = (uint64_t)(-one);
        C17_CHECK_UEQ(wide, 0x00000000FFFFFFFFULL);

        uint32_t zero = (uint32_t)c17_opaque_int(0);
        C17_CHECK_UEQ((uint64_t)(-zero), 0ULL);

        /* Also test unsigned 32-bit subtraction zero-extending to 64-bit */
        uint32_t u1 = (uint32_t)c17_opaque_int(3);
        uint32_t u2 = (uint32_t)c17_opaque_int(5);
        uint64_t diff = (uint64_t)(u1 - u2);
        C17_CHECK_UEQ(diff, 0x00000000FFFFFFFEULL);
    }

    /* Signed division & remainder truncation toward zero (C17 §6.5.5 p6) */
    {
        C17_CHECK_EQ(sdiv32(c17_opaque_int(-7), c17_opaque_int(2)), -3);
        C17_CHECK_EQ(smod32(c17_opaque_int(-7), c17_opaque_int(2)), -1);
        C17_CHECK_EQ(sdiv32(c17_opaque_int(7), c17_opaque_int(-2)), -3);
        C17_CHECK_EQ(smod32(c17_opaque_int(7), c17_opaque_int(-2)), 1);
        C17_CHECK_EQ(sdiv32(c17_opaque_int(-7), c17_opaque_int(-2)), 3);
        C17_CHECK_EQ(smod32(c17_opaque_int(-7), c17_opaque_int(-2)), -1);
        C17_CHECK_EQ(sdiv32(c17_opaque_int(INT_MIN), c17_opaque_int(2)), -1073741824);
        C17_CHECK_EQ(smod32(c17_opaque_int(INT_MIN), c17_opaque_int(3)), -2);
        C17_CHECK_EQ(sdiv64(c17_opaque_ll(-1000000000007LL), c17_opaque_ll(10LL)), -100000000000LL);
        C17_CHECK_EQ(smod64(c17_opaque_ll(-1000000000007LL), c17_opaque_ll(10LL)), -7LL);
        C17_CHECK_EQ(smod64(c17_opaque_ll(-1LL), c17_opaque_ll(1LL)), 0LL);
    }

    /* double/float <-> uint64_t values >= 2^63 */
    {
        /* 2^63 = 9223372036854775808.0; ULP at 2^63 for double is 2048.0 */
        double d_hi = c17_opaque_dbl(9223372036854775808.0);
        double d_hi2 = c17_opaque_dbl(9223372036854775808.0 + 2048.0);
        C17_CHECK_UEQ(dbl_to_u64(d_hi), 0x8000000000000000ULL);
        C17_CHECK_UEQ(dbl_to_u64(d_hi2), 0x8000000000000800ULL);

        /* ULP at 2^63 for float is 2^40 = 1099511627776.0f */
        float f_hi = c17_opaque_flt(9223372036854775808.0f);
        float f_hi2 = c17_opaque_flt(9223372036854775808.0f + 1099511627776.0f);
        C17_CHECK_UEQ(flt_to_u64(f_hi), 0x8000000000000000ULL);
        C17_CHECK_UEQ(flt_to_u64(f_hi2), 0x8000010000000000ULL);

        /* uint64_t >= 2^63 -> double / float (must be positive, NOT negative!) */
        uint64_t u_hi = c17_opaque_ull(0x8000000000000800ULL);
        double got_d = u64_to_dbl(u_hi);
        C17_CHECK(got_d > 0.0);
        C17_CHECK_DBL(got_d, 9223372036854775808.0 + 2048.0, 0.0);

        uint64_t u_max = c17_opaque_ull(UINT64_MAX);
        double got_max = u64_to_dbl(u_max);
        C17_CHECK(got_max > 0.0);
        C17_CHECK_DBL(got_max, 18446744073709551616.0, 0.0);

        float got_f = u64_to_flt(c17_opaque_ull(0x8000010000000000ULL));
        C17_CHECK(got_f > 0.0f);
        C17_CHECK_DBL((double)got_f, 9223372036854775808.0 + 1099511627776.0, 0.0);
    }

    /* Non-lvalue struct array member subscript (C17 §6.5.2.1 / §6.3.2.1 p3) */
    {
        int idx = c17_opaque_int(2);
        int val = make_arr_box(c17_opaque_int(5)).arr[idx];
        C17_CHECK_EQ(val, 25);
    }

    /* *p++->ptr ordering: postfix -> and ++ bind tighter than unary * */
    {
        int x = 101;
        int y = 202;
        struct node nodes[2] = { { &x, 1 }, { &y, 2 } };
        struct node *p = nodes;
        int v1 = *p++->ptr;
        int v2 = *p->ptr;
        C17_CHECK_EQ(v1, 101);
        C17_CHECK_EQ(v2, 202);
        C17_CHECK_EQ(p->tag, 2);
    }

    /* Ternary with narrow signed/unsigned arms */
    {
        int cond = c17_opaque_int(1);
        signed char sc = (signed char)c17_opaque_int(-5);
        unsigned short us = (unsigned short)c17_opaque_int(10);
        int r = cond ? sc : us;
        C17_CHECK_EQ(r, -5);
    }

    C17_END();
}
