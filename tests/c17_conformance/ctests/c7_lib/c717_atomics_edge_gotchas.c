/*
 * C17 §7.17: Atomic gotchas — sub-word atomics (_Atomic uint8_t, _Atomic uint16_t,
 * _Atomic _Bool) with 8-bit/16-bit wraparound and CAS, plus atomic pointer
 * arithmetic scaling (atomic_fetch_add(&p, 2) must advance by 2 * sizeof(*p)!).
 */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

int main(void) {
    C17_BEGIN("c717_atomics_edge_gotchas", "Sub-word atomics (i8/i16/_Bool) and atomic pointer scaling");

    /* 8-bit unsigned atomic wraparound & CAS */
    _Atomic uint8_t au8 = 254u;
    C17_CHECK_EQ(atomic_fetch_add(&au8, 3u), 254u);
    C17_CHECK_EQ(atomic_load(&au8), 1u); /* (254 + 3) mod 256 == 1 */
    uint8_t exp8 = 1u;
    C17_CHECK_EQ(atomic_compare_exchange_strong(&au8, &exp8, 0xA5u), 1);
    C17_CHECK_EQ(atomic_load(&au8), 0xA5u);
    exp8 = 0x11u;
    C17_CHECK_EQ(atomic_compare_exchange_strong(&au8, &exp8, 0xFFu), 0);
    C17_CHECK_EQ(exp8, 0xA5u);

    /* 16-bit unsigned atomic wraparound & CAS */
    _Atomic uint16_t au16 = 65530u;
    C17_CHECK_EQ(atomic_fetch_add(&au16, 10u), 65530u);
    C17_CHECK_EQ(atomic_load(&au16), 4u);
    uint16_t exp16 = 4u;
    C17_CHECK_EQ(atomic_compare_exchange_strong(&au16, &exp16, 0x1234u), 1);
    C17_CHECK_UEQ(atomic_load(&au16), 0x1234u);

    /* _Atomic _Bool normalization */
    _Atomic _Bool ab = 0;
    atomic_store(&ab, 256);
    C17_CHECK_EQ(atomic_load(&ab), 1);
    _Bool old_b = atomic_exchange(&ab, 0);
    C17_CHECK_EQ(old_b, 1);
    C17_CHECK_EQ(atomic_load(&ab), 0);

    /* Atomic pointer arithmetic MUST scale by sizeof(*p)! */
    int arr[6] = { 10, 20, 30, 40, 50, 60 };
    _Atomic(int *) ap = &arr[1];
    int *prev_p = atomic_fetch_add(&ap, 3);
    C17_CHECK(prev_p == &arr[1]);
    C17_CHECK(atomic_load(&ap) == &arr[4]);
    C17_CHECK_EQ(*atomic_load(&ap), 50);

    int *prev_p2 = atomic_fetch_sub(&ap, 2);
    C17_CHECK(prev_p2 == &arr[4]);
    C17_CHECK(atomic_load(&ap) == &arr[2]);
    C17_CHECK_EQ(*atomic_load(&ap), 30);

    /* ++ / -- and atomic_fetch_add on _Atomic pointer */
    C17_CHECK(++ap == &arr[3]);
    C17_CHECK(ap-- == &arr[3]);
    C17_CHECK(atomic_load(&ap) == &arr[2]);
    atomic_fetch_add(&ap, 3);
    C17_CHECK(atomic_load(&ap) == &arr[5]);

    C17_END();
}
