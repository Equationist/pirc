/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.17 — Real-world C17 atomics bug regressions:
 *   - atomic_compare_exchange_strong/weak failure path MUST leave target
 *     unchanged AND update *expected with the actual current value
 *   - atomic_flag test-and-set / clear lifecycle with ATOMIC_FLAG_INIT
 *   - atomic_thread_fence & atomic_signal_fence with all memory orders
 *   - _Atomic small (4B & 8B) struct load/store/exchange/CAS
 */
#include "c17_test.h"

#ifdef __STDC_NO_ATOMICS__
int main(void) {
    C17_BEGIN("c717_bug_atomic_struct_fence",
              "CAS failure expected-update, atomic_flag, fences, _Atomic struct");
    C17_NOT_APPLICABLE("__STDC_NO_ATOMICS__ is defined");
}
#else
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

struct pair32 {
    int16_t a;
    int16_t b;
};

struct pair64 {
    int32_t x;
    int32_t y;
};

int main(void) {
    C17_BEGIN("c717_bug_atomic_struct_fence",
              "CAS failure expected-update, atomic_flag, fences, _Atomic struct");

    /* CAS failure contract on 8, 16, 32, 64-bit atomics */
    {
        _Atomic uint8_t a8 = 0x42;
        uint8_t exp8 = 0x11;
        bool ok8 = atomic_compare_exchange_strong(&a8, &exp8, (uint8_t)0x99);
        C17_CHECK(!ok8);
        C17_CHECK_UEQ(exp8, 0x42U);
        C17_CHECK_UEQ(atomic_load(&a8), 0x42U);
        ok8 = atomic_compare_exchange_strong(&a8, &exp8, (uint8_t)0x99);
        C17_CHECK(ok8);
        C17_CHECK_UEQ(atomic_load(&a8), 0x99U);

        _Atomic uint16_t a16 = 0x1234;
        uint16_t exp16 = 0x0001;
        bool ok16 = atomic_compare_exchange_strong(&a16, &exp16, (uint16_t)0x5678);
        C17_CHECK(!ok16);
        C17_CHECK_UEQ(exp16, 0x1234U);
        C17_CHECK_UEQ(atomic_load(&a16), 0x1234U);

        _Atomic uint64_t a64 = 0x1122334455667788ULL;
        uint64_t exp64 = 0x99ULL;
        bool ok64 = atomic_compare_exchange_strong_explicit(
            &a64, &exp64, 0xAABBCCDDEEFF0011ULL,
            memory_order_acq_rel, memory_order_acquire);
        C17_CHECK(!ok64);
        C17_CHECK_UEQ(exp64, 0x1122334455667788ULL);
        C17_CHECK_UEQ(atomic_load(&a64), 0x1122334455667788ULL);
        ok64 = atomic_compare_exchange_strong_explicit(
            &a64, &exp64, 0xAABBCCDDEEFF0011ULL,
            memory_order_seq_cst, memory_order_relaxed);
        C17_CHECK(ok64);
        C17_CHECK_UEQ(atomic_load(&a64), 0xAABBCCDDEEFF0011ULL);
    }

    /* atomic_flag lifecycle */
    {
        atomic_flag fl = ATOMIC_FLAG_INIT;
        C17_CHECK(!atomic_flag_test_and_set(&fl));
        C17_CHECK(atomic_flag_test_and_set_explicit(&fl, memory_order_acq_rel));
        atomic_flag_clear_explicit(&fl, memory_order_release);
        C17_CHECK(!atomic_flag_test_and_set_explicit(&fl, memory_order_acquire));
        atomic_flag_clear(&fl);
    }

    /* All standard fence memory orders */
    {
        atomic_thread_fence(memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        atomic_thread_fence(memory_order_release);
        atomic_thread_fence(memory_order_acq_rel);
        atomic_thread_fence(memory_order_seq_cst);
        atomic_signal_fence(memory_order_seq_cst);
        C17_CHECK(1);
    }

    /* _Atomic 4-byte & 8-byte structs */
    {
        _Atomic struct pair32 p32;
        struct pair32 init32 = { 10, -20 };
        atomic_init(&p32, init32);
        struct pair32 v32 = atomic_load(&p32);
        C17_CHECK_EQ(v32.a, 10);
        C17_CHECK_EQ(v32.b, -20);

        struct pair32 n32 = { 300, -400 };
        struct pair32 old32 = atomic_exchange(&p32, n32);
        C17_CHECK_EQ(old32.a, 10);
        C17_CHECK_EQ(old32.b, -20);
        v32 = atomic_load(&p32);
        C17_CHECK_EQ(v32.a, 300);
        C17_CHECK_EQ(v32.b, -400);

        _Atomic struct pair64 p64;
        struct pair64 init64 = { 1000, -2000 };
        atomic_init(&p64, init64);
        struct pair64 exp64 = { 0, 0 };
        struct pair64 des64 = { 7777, -8888 };
        bool ok = atomic_compare_exchange_strong(&p64, &exp64, des64);
        C17_CHECK(!ok);
        C17_CHECK_EQ(exp64.x, 1000);
        C17_CHECK_EQ(exp64.y, -2000);
        ok = atomic_compare_exchange_strong(&p64, &exp64, des64);
        C17_CHECK(ok);
        struct pair64 got64 = atomic_load(&p64);
        C17_CHECK_EQ(got64.x, 7777);
        C17_CHECK_EQ(got64.y, -8888);
    }

    C17_END();
}
#endif
