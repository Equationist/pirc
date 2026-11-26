/* C17 §7.17: Non-power-of-two _Atomic struct sizes (3B->4B, 5B->8B, 12B->16B)
 * - Verifies sizeof/alignof match Darwin/AAPCS64/SysV ABI
 * - Verifies atomic_store, atomic_load, atomic_exchange, atomic_compare_exchange_strong
 *   never over-read or over-write adjacent canary fields on unqualified source/expected objects */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdalign.h>
#include <stdint.h>

struct S3 { uint8_t b[3]; };
struct S5 { uint8_t b[5]; };
struct S12 { uint8_t b[12]; };

struct GuardS3 {
    struct S3 val;
    uint8_t canary;
};

struct GuardS5 {
    struct S5 val;
    uint8_t canary[3];
};

struct GuardS12 {
    struct S12 val;
    uint8_t canary[4];
};

int main(void) {
    /* 1. 3-byte struct -> 4-byte _Atomic */
    C17_CHECK(sizeof(_Atomic struct S3) == 4);
    C17_CHECK(alignof(_Atomic struct S3) == 4);
    _Atomic struct S3 a3;
    struct GuardS3 g3 = { {{ 0x11, 0x22, 0x33 }}, 0xEE };
    atomic_store(&a3, g3.val);
    struct S3 r3 = atomic_load(&a3);
    C17_CHECK(r3.b[0] == 0x11 && r3.b[1] == 0x22 && r3.b[2] == 0x33);

    struct GuardS3 n3 = { {{ 0x44, 0x55, 0x66 }}, 0xDD };
    struct S3 old3 = atomic_exchange(&a3, n3.val);
    C17_CHECK(old3.b[0] == 0x11 && old3.b[1] == 0x22 && old3.b[2] == 0x33);

    /* Failing CAS must not overwrite g3.canary or mutate a3 */
    g3.val.b[0] = 0x99;
    struct S3 w3 = {{ 0xAA, 0xBB, 0xCC }};
    C17_CHECK(!atomic_compare_exchange_strong(&a3, &g3.val, w3));
    C17_CHECK(g3.canary == 0xEE);
#ifdef __SLIMCC_PIR__
    C17_CHECK(g3.val.b[0] == 0x44 && g3.val.b[1] == 0x55 && g3.val.b[2] == 0x66);
#endif
    r3 = atomic_load(&a3);
    C17_CHECK(r3.b[0] == 0x44 && r3.b[1] == 0x55 && r3.b[2] == 0x66);

    /* Succeeding CAS */
    struct GuardS3 exp3 = { {{ 0x44, 0x55, 0x66 }}, 0x77 };
    C17_CHECK(atomic_compare_exchange_strong(&a3, &exp3.val, w3));
    C17_CHECK(exp3.canary == 0x77);
    r3 = atomic_load(&a3);
    C17_CHECK(r3.b[0] == 0xAA && r3.b[1] == 0xBB && r3.b[2] == 0xCC);

    /* 2. 5-byte struct -> 8-byte _Atomic */
    C17_CHECK(sizeof(_Atomic struct S5) == 8);
    C17_CHECK(alignof(_Atomic struct S5) == 8);
    _Atomic struct S5 a5;
    struct GuardS5 g5 = { {{ 1, 2, 3, 4, 5 }}, { 0xA1, 0xA2, 0xA3 } };
    atomic_store(&a5, g5.val);
    g5.val.b[0] = 99;
    struct S5 w5 = {{ 10, 20, 30, 40, 50 }};
    C17_CHECK(!atomic_compare_exchange_strong(&a5, &g5.val, w5));
    C17_CHECK(g5.canary[0] == 0xA1 && g5.canary[1] == 0xA2 && g5.canary[2] == 0xA3);
#ifdef __SLIMCC_PIR__
    C17_CHECK(g5.val.b[0] == 1 && g5.val.b[4] == 5);
#endif
    struct GuardS5 exp5 = { {{ 1, 2, 3, 4, 5 }}, { 0xB1, 0xB2, 0xB3 } };
    C17_CHECK(atomic_compare_exchange_strong(&a5, &exp5.val, w5));
    C17_CHECK(exp5.canary[0] == 0xB1 && exp5.canary[2] == 0xB3);
    struct S5 r5 = atomic_load(&a5);
    C17_CHECK(r5.b[0] == 10 && r5.b[4] == 50);

    /* 3. 12-byte struct -> 16-byte _Atomic (lock-backed in slimcc-pir) */
    C17_CHECK(sizeof(_Atomic struct S12) == 16);
    C17_CHECK(alignof(_Atomic struct S12) == 16);
    _Atomic struct S12 a12;
    struct GuardS12 g12 = {
        {{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }},
        { 0xC1, 0xC2, 0xC3, 0xC4 }
    };
    atomic_store(&a12, g12.val);
    g12.val.b[0] = 77;
    struct S12 w12 = {{ 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 }};
    C17_CHECK(!atomic_compare_exchange_strong(&a12, &g12.val, w12));
    C17_CHECK(g12.canary[0] == 0xC1 && g12.canary[3] == 0xC4);
#ifdef __SLIMCC_PIR__
    C17_CHECK(g12.val.b[0] == 1 && g12.val.b[11] == 12);
#endif
    struct GuardS12 exp12 = {
        {{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 }},
        { 0xD1, 0xD2, 0xD3, 0xD4 }
    };
    C17_CHECK(atomic_compare_exchange_strong(&a12, &exp12.val, w12));
    C17_CHECK(exp12.canary[0] == 0xD1 && exp12.canary[3] == 0xD4);
    struct S12 r12 = atomic_load(&a12);
    C17_CHECK(r12.b[0] == 21 && r12.b[11] == 32);

    C17_END();
}
