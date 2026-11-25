/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.17 — Adversarial ARM64/x64 register allocator
 * stress test for atomic CAS and atomic RMW under extreme GP register pressure
 * (30+ live variables) + >4KB stack frame (so spill slots exceed 255B offset).
 * Targets scratch-register collision in MI_A64_ATOMIC_CAS / MI_A64_ATOMIC_RMW.
 */
#include <stdatomic.h>
#include <stdint.h>
#include "c17_test.h"

static int64_t cas_under_extreme_pressure(
    _Atomic uint64_t *target64,
    _Atomic uint32_t *target32,
    _Atomic uint16_t *target16,
    _Atomic uint8_t  *target8,
    uint64_t exp64_in, uint64_t des64_in,
    uint32_t exp32_in, uint32_t des32_in,
    uint16_t exp16_in, uint16_t des16_in,
    uint8_t  exp8_in,  uint8_t  des8_in,
    int64_t seed)
{
    /* Large fixed stack buffer (>4KB) forces spill slots past 255B FP offset */
    int64_t big[600];
    big[0] = seed + 1;
    big[299] = seed + 2;
    big[599] = seed + 3;

    /* 28 simultaneously live GP variables across every atomic op (forces heavy spilling) */
    int64_t v00 = seed + 10,  v01 = seed + 11,  v02 = seed + 12,  v03 = seed + 13;
    int64_t v04 = seed + 14,  v05 = seed + 15,  v06 = seed + 16,  v07 = seed + 17;
    int64_t v08 = seed + 18,  v09 = seed + 19,  v10 = seed + 20,  v11 = seed + 21;
    int64_t v12 = seed + 22,  v13 = seed + 23,  v14 = seed + 24,  v15 = seed + 25;
    int64_t v16 = seed + 26,  v17 = seed + 27,  v18 = seed + 28,  v19 = seed + 29;
    int64_t v20 = seed + 30,  v21 = seed + 31,  v22 = seed + 32,  v23 = seed + 33;
    int64_t v24 = seed + 34,  v25 = seed + 35,  v26 = seed + 36,  v27 = seed + 37;

    /* 1. 64-bit CAS (succeeding or failing depending on exp64_in) */
    uint64_t e64 = exp64_in;
    int ok64 = atomic_compare_exchange_strong(target64, &e64, des64_in);

    /* 2. 32-bit CAS */
    uint32_t e32 = exp32_in;
    int ok32 = atomic_compare_exchange_strong(target32, &e32, des32_in);

    /* 3. 16-bit CAS */
    uint16_t e16 = exp16_in;
    int ok16 = atomic_compare_exchange_strong(target16, &e16, des16_in);

    /* 4. 8-bit CAS */
    uint8_t e8 = exp8_in;
    int ok8 = atomic_compare_exchange_strong(target8, &e8, des8_in);

    /* 5. Atomic RMW ops under same extreme pressure */
    uint64_t old_add = atomic_fetch_add(target64, (uint64_t)(v00 & 7));
    uint32_t old_xor = atomic_fetch_xor(target32, (uint32_t)(v01 & 15));
    uint16_t old_or  = atomic_fetch_or(target16,  (uint16_t)(v02 & 3));
    uint8_t  old_sub = atomic_fetch_sub(target8,  (uint8_t)1u);

    /* Mix all 28 live variables + big[] FIRST so v00..v27 end BEFORE
     * target/exp/des (forcing linear scan to spill target/exp/des!). */
    int64_t sum = v00 + v01 + v02 + v03 + v04 + v05 + v06 + v07
                + v08 + v09 + v10 + v11 + v12 + v13 + v14 + v15
                + v16 + v17 + v18 + v19 + v20 + v21 + v22 + v23
                + v24 + v25 + v26 + v27;
    sum += big[0] + big[299] + big[599];
    sum += (int64_t)(ok64 * 1000 + ok32 * 100 + ok16 * 10 + ok8);
    sum += (int64_t)(e64 + e32 + e16 + e8);
    sum += (int64_t)(old_add + old_xor + old_or + old_sub);
    /* Keep exp/des/targets live past all 28 vars so they are chosen as spill victims */
    sum += (int64_t)((exp64_in & 1u) ^ (des64_in & 1u) ^ ((uintptr_t)target64 & 0u));
    sum += (int64_t)((exp32_in & 1u) ^ (des32_in & 1u) ^ ((uintptr_t)target32 & 0u));
    sum += (int64_t)((exp16_in & 1u) ^ (des16_in & 1u) ^ ((uintptr_t)target16 & 0u));
    sum += (int64_t)((exp8_in  & 1u) ^ (des8_in  & 1u) ^ ((uintptr_t)target8  & 0u));
    return sum;
}

static int64_t (*volatile g_cas_stress)(
    _Atomic uint64_t *, _Atomic uint32_t *, _Atomic uint16_t *, _Atomic uint8_t *,
    uint64_t, uint64_t, uint32_t, uint32_t, uint16_t, uint16_t, uint8_t, uint8_t,
    int64_t) = cas_under_extreme_pressure;

int main(void) {
    C17_BEGIN("c717_bug_arm64_atomic_cas_rmw_high_pressure",
              "Atomic CAS & RMW under 28+ spilled GP regs & >4KB stack frame");

    /* Case A: ALL 4 CAS calls MUST FAIL (expected != actual)!
     * If dst0 collides with expected in scratch reg, CAS falsely succeeds! */
    {
        _Atomic uint64_t t64 = 100u;
        _Atomic uint32_t t32 = 200u;
        _Atomic uint16_t t16 = 300u;
        _Atomic uint8_t  t8  = 50u;

        int64_t seed = c17_opaque_int(1);
        /* v00..v27 = 11..38 => sum = 28*(11+38)/2 = 686
         * big = 2+3+4 = 9
         * all 4 CAS fail => ok=0; e64=100, e32=200, e16=300, e8=50 => 650
         * RMW olds: old_add=100 (t64->100+(11&7)=103), old_xor=200 (t32->200^12=196),
         *           old_or=300 (t16->300|1=301), old_sub=50 (t8->49) => 650
         * total = 686 + 9 + 0 + 650 + 650 = 1995 */
        int64_t r = g_cas_stress(&t64, &t32, &t16, &t8,
                                 999u, 777u,   /* mismatch 64 */
                                 888u, 666u,   /* mismatch 32 */
                                 555u, 444u,   /* mismatch 16 */
                                 99u,  88u,    /* mismatch 8  */
                                 seed);
        C17_CHECK_EQ(r, 1997);
        C17_CHECK_EQ(atomic_load(&t64), 103u);
        C17_CHECK_EQ(atomic_load(&t32), 196u);
        C17_CHECK_EQ(atomic_load(&t16), 301u);
        C17_CHECK_EQ(atomic_load(&t8),  49u);
    }

    /* Case B: ALL 4 CAS calls MUST SUCCEED! */
    {
        _Atomic uint64_t t64 = 100u;
        _Atomic uint32_t t32 = 200u;
        _Atomic uint16_t t16 = 300u;
        _Atomic uint8_t  t8  = 50u;

        int64_t seed = c17_opaque_int(1);
        /* v00..v27 = 686, big = 9, ok = 1111
         * e64=100, e32=200, e16=300, e8=50 => 650
         * CAS sets: t64=500, t32=600, t16=700, t8=80
         * RMW olds: 500 + 600 + 700 + 80 = 1880
         * Final targets: t64=503, t32=600^12=596, t16=700|1=701, t8=79
         * total = 686 + 9 + 1111 + 650 + 1880 = 4336 */
        int64_t r = g_cas_stress(&t64, &t32, &t16, &t8,
                                 100u, 500u,
                                 200u, 600u,
                                 300u, 700u,
                                 50u,  80u,
                                 seed);
        C17_CHECK_EQ(r, 4336);
        C17_CHECK_EQ(atomic_load(&t64), 503u);
        C17_CHECK_EQ(atomic_load(&t32), 596u);
        C17_CHECK_EQ(atomic_load(&t16), 701u);
        C17_CHECK_EQ(atomic_load(&t8),  79u);
    }

    /* Case C: Mixed (64 & 16 succeed, 32 & 8 fail) */
    {
        _Atomic uint64_t t64 = 10u;
        _Atomic uint32_t t32 = 20u;
        _Atomic uint16_t t16 = 30u;
        _Atomic uint8_t  t8  = 40u;

        int64_t seed = c17_opaque_int(1);
        /* v00..v27=686, big=9, ok=1010
         * e: 10+20+30+40=100
         * after CAS: t64=50, t32=20, t16=70, t8=40 => RMW olds = 180
         * total = 686 + 9 + 1010 + 100 + 180 = 1985 */
        int64_t r = g_cas_stress(&t64, &t32, &t16, &t8,
                                 10u, 50u,
                                 99u, 60u,
                                 30u, 70u,
                                 99u, 80u,
                                 seed);
        C17_CHECK_EQ(r, 1987);
        C17_CHECK_EQ(atomic_load(&t64), 53u);
        C17_CHECK_EQ(atomic_load(&t32), 20u ^ 12u);
        C17_CHECK_EQ(atomic_load(&t16), 71u);
        C17_CHECK_EQ(atomic_load(&t8),  39u);
    }

    C17_END();
}
