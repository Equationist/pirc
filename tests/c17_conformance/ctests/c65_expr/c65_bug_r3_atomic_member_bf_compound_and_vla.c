/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §6.7 / §7.17 — Round 3 regressions:
 *   1. atomic_load(&agg).member & atomic_exchange(&agg, v).member (4B & 20B)
 *   2. Bitfield compound assignment integer promotions (u31 -> int, u32 -> uint)
 *   3. Nested & over-aligned _Alignas(64) VLAs with loop stackrestore
 *   4. Static constant pointer subtraction (&a[5] - &a[2])
 *   5. 64-bit constant division (-2147483648LL) / (-1LL)
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct SmallPair {
    int16_t a;
    int16_t b;
};

struct BigAgg {
    int v[5];
};

static int g_arr[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static ptrdiff_t g_pdiff = &g_arr[7] - &g_arr[2];
static int g_pdiff_i = (int)(&g_arr[9] - &g_arr[4]);

/* 64-bit constant eval must NOT treat INT32_MIN as INT64_MIN */
static const long long g_div_i32min_ll = (-2147483648LL) / (-1LL);
_Static_assert(((-2147483648LL) / (-1LL)) == 2147483648LL,
               "(-2147483648LL)/(-1LL) must equal +2147483648LL");

struct BfPromo {
    unsigned int u31 : 31;
    unsigned int u32 : 32;
    unsigned int u7  : 7;
    signed int   s5  : 5;
};

static int test_nested_overaligned_vla(int n) {
    int sum = 0;
    for (int iter = 0; iter < 500; iter++) {
        _Alignas(64) int outer[n];
        if (((uintptr_t)outer & 63u) != 0u)
            return -1;
        outer[0] = iter + 1;
        outer[n - 1] = iter + 2;
        {
            _Alignas(64) int inner[n + 4];
            if (((uintptr_t)inner & 63u) != 0u)
                return -2;
            inner[0] = outer[0] * 2;
            inner[n + 3] = outer[n - 1] * 3;
            sum += inner[0] + inner[n + 3];
        }
        /* outer must remain intact after inner VLA is stackrestored! */
        if (outer[0] != iter + 1 || outer[n - 1] != iter + 2)
            return -3;
        sum += outer[0] - outer[n - 1];
    }
    return sum;
}

int main(void) {
    C17_BEGIN("c65_bug_r3_atomic_member_bf_compound_and_vla",
              "Atomic agg member/exch, bitfield compound promo, nested aligned VLA, ptr diff, LL div");

    /* 1. Atomic aggregate load & exchange member access */
    _Atomic struct SmallPair asp;
    struct SmallPair sp0 = { 11, 22 };
    struct SmallPair sp1 = { 33, 44 };
    atomic_init(&asp, sp0);
    C17_CHECK_EQ(atomic_load(&asp).a, 11);
    C17_CHECK_EQ(atomic_load(&asp).b, 22);
    C17_CHECK_EQ(atomic_exchange(&asp, sp1).a, 11);
    C17_CHECK_EQ(atomic_load(&asp).b, 44);

    _Atomic struct BigAgg abig;
    struct BigAgg b0 = { { 1, 2, 3, 4, 5 } };
    struct BigAgg b1 = { { 10, 20, 30, 40, 50 } };
    atomic_init(&abig, b0);
    C17_CHECK_EQ(atomic_load(&abig).v[4], 5);
    C17_CHECK_EQ(atomic_exchange(&abig, b1).v[2], 3);
    C17_CHECK_EQ(atomic_load(&abig).v[3], 40);

    /* 2. Bitfield compound assignment promotions (C17 §6.3.1.1p2) */
    struct BfPromo bf = { 10u, 10u, 10u, -6 };
    /* u31 promotes to signed int: 10 / (-2) == -5 -> masked to 31 bits */
    bf.u31 /= c17_opaque_int(-2);
    C17_CHECK_EQ(bf.u31, (unsigned int)(-5) & 0x7fffffffu);
    /* u32 promotes to unsigned int: 10u / (unsigned)(-2) == 0u */
    bf.u32 /= c17_opaque_int(-2);
    C17_CHECK_EQ(bf.u32, 0u);
    /* u7 promotes to signed int: 10 / (-3) == -3 -> masked to 7 bits (125) */
    bf.u7 /= c17_opaque_int(-3);
    C17_CHECK_EQ(bf.u7, (unsigned int)(-3) & 0x7fu);
    /* s5 signed bitfield compound div & post-inc */
    bf.s5 /= c17_opaque_int(-2);
    C17_CHECK_EQ(bf.s5, 3);
    int prev = bf.s5++;
    C17_CHECK_EQ(prev, 3);
    C17_CHECK_EQ(bf.s5, 4);

    /* 3. Nested over-aligned VLAs with stackrestore */
    int vla_sum = test_nested_overaligned_vla(c17_opaque_int(8));
    /* Each iter: 2*(i+1) + 3*(i+2) + (i+1) - (i+2) = 5*i + 7; sum_{i=0..499} = 5*124750 + 3500 = 627250 */
    C17_CHECK_EQ(vla_sum, 627250);

    /* 4. Static constant pointer subtraction */
    C17_CHECK_EQ(g_pdiff, 5);
    C17_CHECK_EQ(g_pdiff_i, 5);

    /* 5. 64-bit constant division of INT32_MIN */
    C17_CHECK_EQ(g_div_i32min_ll, 2147483648LL);

    C17_END();
}
