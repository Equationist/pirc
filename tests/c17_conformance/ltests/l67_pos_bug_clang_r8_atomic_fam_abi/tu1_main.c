/* Bidirectional clang <-> slimcc-pir ABI interop (Round 8):
 * 1B/2B _Atomic structs and 1B/2B FAM structs (SysV x86-64 MEMORY class + ARM64).
 */
#include "c17_test.h"
#include "interop.h"

int slim_recv_r8(int g0, int g1, int g2, int g3, int g4, int g5,
                 R8At1 a1, R8At2 a2, R8Fam1 f1, R8Fam2 f2, int tail) {
    return g0 + g1 + g2 + g3 + g4 + g5 +
           (int)atomic_load(&a1.a) + (int)atomic_load(&a2.a) +
           (int)f1.a + (int)f2.a + tail;
}

R8At1 slim_ret_at1(uint8_t v) {
    R8At1 s;
    atomic_init(&s.a, v);
    return s;
}

R8At2 slim_ret_at2(uint16_t v) {
    R8At2 s;
    atomic_init(&s.a, v);
    return s;
}

int main(void) {
    R8At1 a1;
    R8At2 a2;
    atomic_init(&a1.a, 11);
    atomic_init(&a2.a, 222);
    R8Fam1 f1 = { 33 };
    R8Fam2 f2 = { 444 };

    int r1 = clang_recv_r8(1, 2, 3, 4, 5, 6, a1, a2, f1, f2, 99);
    C17_CHECK(r1 == 1 + 2 + 3 + 4 + 5 + 6 + 11 + 222 + 33 + 444 + 99);

    R8At1 ra1 = clang_ret_at1(77);
    R8At2 ra2 = clang_ret_at2(888);
    C17_CHECK(atomic_load(&ra1.a) == 77);
    C17_CHECK(atomic_load(&ra2.a) == 888);

    C17_CHECK(clang_test_slim_r8() == 1);
    C17_END();
}
