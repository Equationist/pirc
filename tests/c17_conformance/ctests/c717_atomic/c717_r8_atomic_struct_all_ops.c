/* ISO C17 §7.17: _Atomic structs across sizes 1..16 (load, store, exchange, CAS) */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdint.h>

typedef struct { uint8_t a; } S1;
typedef struct { uint8_t a, b; } S2;
typedef struct { uint8_t a, b, c; } S3;
typedef struct { uint16_t a, b; } S4;
typedef struct { uint32_t a, b; } S8;
typedef struct { uint32_t a, b, c; } S12;

int main(void) {
    _Atomic S1 a1;
    _Atomic S2 a2;
    _Atomic S3 a3;
    _Atomic S4 a4;
    _Atomic S8 a8;
    _Atomic S12 a12;
    atomic_init(&a1, ((S1){ 11 }));
    atomic_init(&a2, ((S2){ 1, 2 }));
    atomic_init(&a3, ((S3){ 3, 4, 5 }));
    atomic_init(&a4, ((S4){ 10, 20 }));
    atomic_init(&a8, ((S8){ 100, 200 }));
    atomic_init(&a12, ((S12){ 7, 8, 9 }));

    S1 v1 = atomic_load(&a1);
    C17_CHECK(v1.a == 11);
    S1 o1 = atomic_exchange(&a1, ((S1){ 22 }));
    C17_CHECK(o1.a == 11 && atomic_load(&a1).a == 22);

    S2 v2 = atomic_load(&a2);
    C17_CHECK(v2.a == 1 && v2.b == 2);
    S2 o2 = atomic_exchange(&a2, ((S2){ 33, 44 }));
    C17_CHECK(o2.a == 1 && o2.b == 2);
    v2 = atomic_load(&a2);
    C17_CHECK(v2.a == 33 && v2.b == 44);

    S3 v3 = atomic_load(&a3);
    C17_CHECK(v3.a == 3 && v3.b == 4 && v3.c == 5);
    atomic_store(&a3, ((S3){ 6, 7, 8 }));
    v3 = atomic_load(&a3);
    C17_CHECK(v3.a == 6 && v3.b == 7 && v3.c == 8);

    S4 o4 = atomic_exchange(&a4, ((S4){ 30, 40 }));
    C17_CHECK(o4.a == 10 && o4.b == 20);
    S4 exp4 = { 30, 40 };
    C17_CHECK(atomic_compare_exchange_strong(&a4, &exp4, ((S4){ 50, 60 })));
    S4 r4 = atomic_load(&a4);
    C17_CHECK(r4.a == 50 && r4.b == 60);

    S8 o8 = atomic_exchange(&a8, ((S8){ 300, 400 }));
    C17_CHECK(o8.a == 100 && o8.b == 200);
    S8 exp8 = { 300, 400 };
    C17_CHECK(atomic_compare_exchange_strong(&a8, &exp8, ((S8){ 500, 600 })));
    S8 r8 = atomic_load(&a8);
    C17_CHECK(r8.a == 500 && r8.b == 600);

    S12 o12 = atomic_exchange(&a12, ((S12){ 17, 18, 19 }));
    C17_CHECK(o12.a == 7 && o12.b == 8 && o12.c == 9);
    S12 r12 = atomic_load(&a12);
    C17_CHECK(r12.a == 17 && r12.b == 18 && r12.c == 19);

    C17_END();
}
