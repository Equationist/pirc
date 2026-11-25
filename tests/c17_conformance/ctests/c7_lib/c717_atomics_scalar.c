/*
 * C17 §7.17 & §6.7.2.4: _Atomic types and <stdatomic.h> single-threaded
 * semantics — scalar, 64-bit, float, pointer atomics, CAS strong/weak,
 * atomic_fetch_*, compound ops, fences, and atomic_flag.
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
    C17_BEGIN("c717_atomics_scalar", "C17 _Atomic & <stdatomic.h>: load/store/xchg/CAS/fetch/flag");

    /* Basic load/store, direct C17 init, atomic_init */
    _Atomic int ai = 10;
    C17_CHECK_EQ(atomic_load(&ai), 10);
    atomic_store(&ai, 25);
    C17_CHECK_EQ(atomic_load_explicit(&ai, memory_order_acquire), 25);
    atomic_store_explicit(&ai, 30, memory_order_release);
    C17_CHECK_EQ(ai, 30);

    _Atomic(long long) all;
    atomic_init(&all, 1000000000000LL);
    C17_CHECK_EQ(atomic_load(&all), 1000000000000LL);

    /* atomic_exchange */
    int prev = atomic_exchange(&ai, 42);
    C17_CHECK_EQ(prev, 30);
    C17_CHECK_EQ(atomic_load(&ai), 42);

    /* atomic_compare_exchange_strong: success and failure (must update *expected!) */
    int expected = 42;
    bool ok1 = atomic_compare_exchange_strong(&ai, &expected, 99);
    C17_CHECK_EQ(ok1, 1);
    C17_CHECK_EQ(atomic_load(&ai), 99);

    expected = 123; /* mismatch! */
    bool ok2 = atomic_compare_exchange_strong(&ai, &expected, 555);
    C17_CHECK_EQ(ok2, 0);
    C17_CHECK_EQ(expected, 99); /* MUST be updated to current value! */
    C17_CHECK_EQ(atomic_load(&ai), 99);

    /* atomic_fetch_{add,sub,or,xor,and} return PREVIOUS value */
    C17_CHECK_EQ(atomic_fetch_add(&ai, 1), 99);
    C17_CHECK_EQ(atomic_load(&ai), 100);
    C17_CHECK_EQ(atomic_fetch_sub(&ai, 36), 100);
    C17_CHECK_EQ(atomic_load(&ai), 64);
    C17_CHECK_EQ(atomic_fetch_or(&ai, 0x0F), 64);
    C17_CHECK_EQ(atomic_load(&ai), 0x4F);
    C17_CHECK_EQ(atomic_fetch_xor(&ai, 0x05), 0x4F);
    C17_CHECK_EQ(atomic_load(&ai), 0x4A);
    C17_CHECK_EQ(atomic_fetch_and(&ai, 0xF0), 0x4A);
    C17_CHECK_EQ(atomic_load(&ai), 0x40);

    /* Compound assignment and ++/-- on _Atomic lvalues */
    C17_CHECK_EQ(++ai, 0x41);
    C17_CHECK_EQ(ai++, 0x41);
    C17_CHECK_EQ(ai, 0x42);
    ai += 8;
    C17_CHECK_EQ(ai, 0x4A);

    /* 64-bit atomic_fetch_add and CAS */
    C17_CHECK_EQ(atomic_fetch_add(&all, 500LL), 1000000000000LL);
    C17_CHECK_EQ(atomic_load(&all), 1000000000500LL);

    /* Atomic pointer */
    int arr[4] = { 10, 20, 30, 40 };
    _Atomic(int *) ap = &arr[0];
    C17_CHECK_EQ(*atomic_load(&ap), 10);
    atomic_store(&ap, &arr[2]);
    C17_CHECK_EQ(*atomic_load(&ap), 30);

    /* Atomic double */
    _Atomic double ad = 1.5;
    double old_d = atomic_exchange(&ad, 3.25);
    C17_CHECK_DBL(old_d, 1.5, 0.0);
    C17_CHECK_DBL(atomic_load(&ad), 3.25, 0.0);

    /* Fences */
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_acq_rel);

    /* atomic_flag with standard object-like ATOMIC_FLAG_INIT */
    atomic_flag fl = ATOMIC_FLAG_INIT;
    C17_CHECK_EQ(atomic_flag_test_and_set(&fl), 0);
    C17_CHECK_EQ(atomic_flag_test_and_set(&fl), 1);
    atomic_flag_clear(&fl);
    C17_CHECK_EQ(atomic_flag_test_and_set_explicit(&fl, memory_order_acquire), 0);
    atomic_flag_clear_explicit(&fl, memory_order_release);

    C17_END();
}
