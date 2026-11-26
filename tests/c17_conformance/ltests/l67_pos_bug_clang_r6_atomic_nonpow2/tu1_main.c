/* Bidirectional clang <-> slimcc-pir Round 6 ABI interop:
 * Non-power-of-two _Atomic struct layout (S3/S5/S12) inside wrapper structs */
#include "c17_test.h"
#include "interop.h"

int main(void) {
    C17_CHECK(sizeof(struct WrapAll) == clang_sizeof_wrap());
    C17_CHECK(offsetof(struct WrapAll, tail) == clang_offsetof_tail());

    struct WrapAll w;
    w.tag = 0x11;
    atomic_store(&w.a3, ((struct S3){{ 0x22, 0x33, 0x44 }}));
    w.mid1 = 0x11223344;
    atomic_store(&w.a5, ((struct S5){{ 1, 2, 3, 4, 5 }}));
    w.mid2 = 0x55667788;
    atomic_store(&w.a12, ((struct S12){{ 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 }}));
    w.tail = 0x12345678;

    C17_CHECK(clang_verify_and_mutate(&w) == 0);

    struct S3 r3 = atomic_load(&w.a3);
    struct S5 r5 = atomic_load(&w.a5);
    C17_CHECK(r3.b[0] == 0xAA && r3.b[1] == 0xBB && r3.b[2] == 0xCC);
    C17_CHECK(r5.b[0] == 9 && r5.b[4] == 13);
    C17_CHECK(w.tail == 0x76543210);

    C17_END();
}
