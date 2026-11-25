/* C17-LDFLAGS: -lpthread */
/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.17 — Regression test for _Atomic odd-sized and
 * >8-byte structs (3B, 5B, 6B, 7B, 12B, 16B):
 *   - Verifies exact-byte-width load/store/exchange/CAS with adjacent canary
 *     bytes (catching OOB 8-byte atomic overwrites and unaligned SIGBUS).
 *   - Verifies CAS failure updates *expected cleanly on non-word sizes.
 *   - Verifies thread-safety under concurrent pthread CAS contention on
 *     _Atomic 12-byte and 16-byte structs.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "c17_test.h"

typedef struct { uint8_t a, b, c; } S3;
typedef struct { uint8_t a[5]; } S5;
typedef struct { uint16_t a, b, c; } S6;
typedef struct { uint8_t a[7]; } S7;
typedef struct { uint32_t a, b, c; } S12;
typedef struct { uint64_t lo, hi; } S16;

struct HasAtomicMembers {
    int tag;
    _Atomic int cnt;
    _Atomic uint16_t u16;
    _Atomic uint8_t u8;
};

static _Atomic S12 g_s12;
static _Atomic S16 g_s16;

static void *cas_worker(void *arg) {
    uint32_t tid = *(uint32_t *)arg;
    for (int i = 0; i < 1000; i++) {
        /* Concurrent atomic_exchange on 12B _Atomic struct: invariant c == a + b */
        S12 next12;
        next12.a = (uint32_t)(i + 1);
        next12.b = (tid + 1u) * 100u;
        next12.c = next12.a + next12.b;
        S12 prev12 = atomic_exchange(&g_s12, next12);
        if (prev12.c != prev12.a + prev12.b) return (void *)1;

        /* Concurrent CAS loop on 16B _Atomic struct: invariant hi == lo * 3 + 7 */
        S16 exp16 = atomic_load(&g_s16);
        for (;;) {
            if (exp16.hi != exp16.lo * 3ULL + 7ULL) return (void *)2;
            S16 des16;
            des16.lo = exp16.lo + 1ULL;
            des16.hi = des16.lo * 3ULL + 7ULL;
            if (atomic_compare_exchange_strong(&g_s16, &exp16, des16))
                break;
        }
    }
    return NULL;
}

int main(void) {
    C17_BEGIN("c717_bug_atomic_odd_and_large_structs",
              "C17 _Atomic 3B/5B/6B/7B/12B/16B structs, canary guard, and pthread CAS");

    /* Canary test for 3-byte _Atomic struct */
    struct {
        uint64_t guard_lo;
        _Atomic S3 s3;
        uint8_t tail_canary[5];
        uint64_t guard_hi;
    } box3;
    box3.guard_lo = 0xDEADBEEFCAFEBABEULL;
    for (int i = 0; i < 5; i++) box3.tail_canary[i] = (uint8_t)(0xA0 + i);
    box3.guard_hi = 0x1122334455667788ULL;

    S3 init3 = { 1, 2, 3 };
    atomic_init(&box3.s3, init3);
    S3 v3 = atomic_load(&box3.s3);
    C17_CHECK_EQ(v3.a, 1);
    C17_CHECK_EQ(v3.b, 2);
    C17_CHECK_EQ(v3.c, 3);

    S3 n3 = { 4, 5, 6 };
    S3 old3 = atomic_exchange(&box3.s3, n3);
    C17_CHECK_EQ(old3.a, 1);
    C17_CHECK_EQ(old3.b, 2);
    C17_CHECK_EQ(old3.c, 3);

    /* Failing CAS must leave box3.s3 unchanged; succeeding CAS updates box3.s3 */
    S3 wrong3 = { 9, 9, 9 };
    S3 des3 = { 7, 8, 9 };
    C17_CHECK(!atomic_compare_exchange_strong(&box3.s3, &wrong3, des3));
    S3 still3 = atomic_load(&box3.s3);
    C17_CHECK_EQ(still3.a, 4);
    C17_CHECK_EQ(still3.b, 5);
    C17_CHECK_EQ(still3.c, 6);

    S3 exp3 = { 4, 5, 6 };
    C17_CHECK(atomic_compare_exchange_strong(&box3.s3, &exp3, des3));
    S3 fin3 = atomic_load(&box3.s3);
    C17_CHECK_EQ(fin3.a, 7);
    C17_CHECK_EQ(fin3.b, 8);
    C17_CHECK_EQ(fin3.c, 9);

    /* Verify adjacent canary bytes were untouched */
    C17_CHECK_EQ(box3.guard_lo, 0xDEADBEEFCAFEBABEULL);
    C17_CHECK_EQ(box3.guard_hi, 0x1122334455667788ULL);
    for (int i = 0; i < 5; i++)
        C17_CHECK_EQ(box3.tail_canary[i], (uint8_t)(0xA0 + i));

    /* 5-byte, 6-byte, and 7-byte _Atomic structs */
    S5 i5 = {{ 1, 2, 3, 4, 5 }}, n5 = {{ 9, 8, 7, 6, 5 }};
    _Atomic S5 s5 = i5;
    S5 v5 = s5;
    C17_CHECK_EQ(v5.a[0], 1);
    C17_CHECK_EQ(v5.a[4], 5);
    s5 = n5;
    S5 v5b = atomic_load(&s5);
    C17_CHECK_EQ(v5b.a[0], 9);
    C17_CHECK_EQ(v5b.a[4], 5);

    S6 i6 = { 100, 200, 300 }, n6 = { 400, 500, 600 };
    _Atomic S6 s6 = i6;
    S6 old6 = atomic_exchange(&s6, n6);
    C17_CHECK_EQ(old6.a, 100);
    C17_CHECK_EQ(old6.c, 300);
    S6 cur6 = atomic_load(&s6);
    C17_CHECK_EQ(cur6.b, 500);

    S7 i7 = {{ 10, 20, 30, 40, 50, 60, 70 }}, n7 = {{ 7, 6, 5, 4, 3, 2, 1 }};
    _Atomic S7 s7 = i7;
    S7 exp7 = i7;
    C17_CHECK(atomic_compare_exchange_strong(&s7, &exp7, n7));
    S7 cur7 = atomic_load(&s7);
    C17_CHECK_EQ(cur7.a[0], 7);
    C17_CHECK_EQ(cur7.a[6], 1);

    /* _Atomic scalar members inside non-atomic struct */
    struct HasAtomicMembers hm = { 7, 10, 65530, 250 };
    C17_CHECK_EQ((hm.cnt += 5), 15);
    C17_CHECK_EQ((uint16_t)(hm.u16 += 10), 4u);
    C17_CHECK_EQ((uint8_t)(hm.u8 += 10), 4u);

    /* Single-threaded 12B atomic CAS & exchange */
    S12 i12 = { 10u, 20u, 30u }, n12 = { 40u, 50u, 90u };
    atomic_store(&g_s12, i12);
    S12 exp12 = i12;
    C17_CHECK(atomic_compare_exchange_strong(&g_s12, &exp12, n12));
    S12 cur12 = atomic_load(&g_s12);
    C17_CHECK_UEQ(cur12.a, 40u);
    C17_CHECK_UEQ(cur12.b, 50u);
    C17_CHECK_UEQ(cur12.c, 90u);

    /* Single-threaded 16B failing CAS must update *expected cleanly */
    S16 init16 = { 0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL };
    atomic_store(&g_s16, init16);
    S16 wrong16 = { 1ULL, 2ULL }, des16 = { 42ULL, 99ULL };
    C17_CHECK(!atomic_compare_exchange_strong(&g_s16, &wrong16, des16));
    C17_CHECK_UEQ(wrong16.lo, 0x1122334455667788ULL);
    C17_CHECK_UEQ(wrong16.hi, 0x99AABBCCDDEEFF00ULL);
    C17_CHECK(atomic_compare_exchange_strong(&g_s16, &wrong16, des16));
    S16 cur16 = atomic_load(&g_s16);
    C17_CHECK_UEQ(cur16.lo, 42ULL);
    C17_CHECK_UEQ(cur16.hi, 99ULL);

    /* Concurrent pthread contention on 12B exchange and 16B CAS */
    S12 z12 = { 0u, 0u, 0u };
    S16 z16 = { 0ULL, 7ULL };
    atomic_store(&g_s12, z12);
    atomic_store(&g_s16, z16);

    pthread_t th[4];
    uint32_t tids[4] = { 0u, 1u, 2u, 3u };
    for (int i = 0; i < 4; i++)
        C17_CHECK_EQ(pthread_create(&th[i], NULL, cas_worker, &tids[i]), 0);
    for (int i = 0; i < 4; i++) {
        void *res = NULL;
        C17_CHECK_EQ(pthread_join(th[i], &res), 0);
        C17_CHECK(res == NULL);
    }

    S12 f12 = atomic_load(&g_s12);
    C17_CHECK_UEQ(f12.c, f12.a + f12.b);

    S16 f16 = atomic_load(&g_s16);
    C17_CHECK_UEQ(f16.lo, 4000ULL);
    C17_CHECK_UEQ(f16.hi, 12007ULL);

    C17_END();
}
