/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.16.2, §7.17 — Atomic compound assignments on
 * float, double, pointers (scaled), _Bool, uint8_t/uint16_t (wrapping),
 * LHS evaluate-once, and memory_order side-effect evaluation in _explicit ops.
 */
#include <stdatomic.h>
#include <stdint.h>
#include "c17_test.h"

static int g_side = 0;

static int bump_idx(int idx) {
    g_side++;
    return idx;
}

static memory_order get_mo(memory_order mo) {
    g_side += 10;
    return mo;
}

int main(void) {
    C17_BEGIN("c717_bug_atomic_compound_fp_ptr_explicit",
              "Atomic compound assign (FP/ptr/_Bool/subword) & _explicit order eval");

    /* 1. _Atomic float & double compound assignment (requires CAS loop) */
    {
        _Atomic float af = 1.5f;
        _Atomic double ad = 10.0;
        float r1 = (af += 2.25f);
        C17_CHECK(r1 == 3.75f && atomic_load(&af) == 3.75f);
        float r2 = (af *= 4.0f);
        C17_CHECK(r2 == 15.0f && atomic_load(&af) == 15.0f);
        float r3 = af++;
        C17_CHECK(r3 == 15.0f && atomic_load(&af) == 16.0f);
        float r4 = --af;
        C17_CHECK(r4 == 15.0f && atomic_load(&af) == 15.0f);

        double d1 = (ad -= 3.5);
        C17_CHECK(d1 == 6.5 && atomic_load(&ad) == 6.5);
        double d2 = (ad /= 2.0);
        C17_CHECK(d2 == 3.25 && atomic_load(&ad) == 3.25);
        double d3 = ++ad;
        C17_CHECK(d3 == 4.25 && atomic_load(&ad) == 4.25);
    }

    /* 2. _Atomic pointer arithmetic (scaled by sizeof(int)) */
    {
        int buf[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
        int *_Atomic ap = &buf[1];
        int *p1 = atomic_fetch_add(&ap, 3);
        C17_CHECK(p1 == &buf[1] && atomic_load(&ap) == &buf[4]);
        int *p2 = ap++;
        C17_CHECK(p2 == &buf[4] && atomic_load(&ap) == &buf[5]);
        int *p3 = --ap;
        C17_CHECK(p3 == &buf[4] && *p3 == 50);
        int *p4 = atomic_fetch_add(&ap, 1);
        C17_CHECK(p4 == &buf[4] && atomic_load(&ap) == &buf[5]);
        int *p5 = atomic_fetch_sub(&ap, 4);
        C17_CHECK(p5 == &buf[5] && atomic_load(&ap) == &buf[1]);
    }

    /* 3. _Atomic _Bool & wrapping sub-word compound ops */
    {
        _Atomic _Bool ab = 0;
        _Bool b0 = ab++;
        C17_CHECK_EQ((int)b0, 0);
        C17_CHECK_EQ((int)atomic_load(&ab), 1);
        _Bool b1 = ab++;
        C17_CHECK_EQ((int)b1, 1);
        C17_CHECK_EQ((int)atomic_load(&ab), 1);
        _Bool b2 = ab--;
        C17_CHECK_EQ((int)b2, 1);
        C17_CHECK_EQ((int)atomic_load(&ab), 0);
        _Bool b3 = (ab |= 1);
        C17_CHECK_EQ((int)b3, 1);
        C17_CHECK_EQ((int)atomic_load(&ab), 1);

        _Atomic uint8_t au8 = 250u;
        uint8_t u8r = (au8 += 10u);
        C17_CHECK_EQ(u8r, 4u);
        C17_CHECK_EQ(atomic_load(&au8), 4u);
        au8 <<= 3;
        C17_CHECK_EQ(atomic_load(&au8), 32u);
        au8 >>= 2;
        C17_CHECK_EQ(atomic_load(&au8), 8u);

        _Atomic uint16_t au16 = 65530u;
        uint16_t u16r = (au16 += 10u);
        C17_CHECK_EQ(u16r, 4u);
        au16 *= 7u;
        C17_CHECK_EQ(atomic_load(&au16), 28u);
        au16 /= 3u;
        C17_CHECK_EQ(atomic_load(&au16), 9u);
    }

    /* 4. LHS evaluated ONCE in atomic compound assignment & _explicit order eval */
    {
        _Atomic int arr[4] = { 1, 2, 3, 4 };
        g_side = 0;
        int res = (arr[bump_idx(2)] += 10);
        C17_CHECK_EQ(res, 13);
        C17_CHECK_EQ(g_side, 1);

        int old = arr[bump_idx(2)]++;
        C17_CHECK_EQ(old, 13);
        C17_CHECK_EQ(atomic_load(&arr[2]), 14);
        C17_CHECK_EQ(g_side, 2);

        /* _explicit memory_order arguments MUST be evaluated */
        g_side = 0;
        atomic_store_explicit(&arr[0], 42, get_mo(memory_order_release));
        C17_CHECK_EQ(g_side, 10);
        int lv = atomic_load_explicit(&arr[0], get_mo(memory_order_acquire));
        C17_CHECK_EQ(lv, 42);
        C17_CHECK_EQ(g_side, 20);
        int fa = atomic_fetch_add_explicit(&arr[0], 8, get_mo(memory_order_acq_rel));
        C17_CHECK_EQ(fa, 42);
        C17_CHECK_EQ(g_side, 30);
        int ex = atomic_exchange_explicit(&arr[0], 99, get_mo(memory_order_seq_cst));
        C17_CHECK_EQ(ex, 50);
        C17_CHECK_EQ(g_side, 40);
        int exp_v = 99;
        _Bool ok = atomic_compare_exchange_strong_explicit(
            &arr[0], &exp_v, 100,
            get_mo(memory_order_acq_rel), get_mo(memory_order_acquire));
        C17_CHECK(ok);
        C17_CHECK_EQ(g_side, 60);
        atomic_thread_fence(get_mo(memory_order_seq_cst));
        C17_CHECK_EQ(g_side, 70);
    }

    C17_END();
}
