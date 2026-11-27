#include "interop.h"

int clang_recv_r8(int g0, int g1, int g2, int g3, int g4, int g5,
                  R8At1 a1, R8At2 a2, R8Fam1 f1, R8Fam2 f2, int tail) {
    return g0 + g1 + g2 + g3 + g4 + g5 +
           (int)atomic_load(&a1.a) + (int)atomic_load(&a2.a) +
           (int)f1.a + (int)f2.a + tail;
}

R8At1 clang_ret_at1(uint8_t v) {
    R8At1 s;
    atomic_init(&s.a, v);
    return s;
}

R8At2 clang_ret_at2(uint16_t v) {
    R8At2 s;
    atomic_init(&s.a, v);
    return s;
}

int clang_test_slim_r8(void) {
    R8At1 a1;
    R8At2 a2;
    atomic_init(&a1.a, 15);
    atomic_init(&a2.a, 250);
    R8Fam1 f1 = { 45 };
    R8Fam2 f2 = { 555 };

    int r = slim_recv_r8(2, 4, 6, 8, 10, 12, a1, a2, f1, f2, 100);
    if (r != 2 + 4 + 6 + 8 + 10 + 12 + 15 + 250 + 45 + 555 + 100)
        return 0;

    R8At1 ra1 = slim_ret_at1(89);
    R8At2 ra2 = slim_ret_at2(999);
    if (atomic_load(&ra1.a) != 89)
        return 0;
    if (atomic_load(&ra2.a) != 999)
        return 0;
    return 1;
}
