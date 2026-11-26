#include "interop.h"

size_t clang_sizeof_wrap(void) {
    return sizeof(struct WrapAll);
}

size_t clang_offsetof_tail(void) {
    return offsetof(struct WrapAll, tail);
}

int32_t clang_verify_and_mutate(struct WrapAll *w) {
    if (w->tag != 0x11) return 1;
    if (w->mid1 != 0x11223344) return 2;
    if (w->mid2 != 0x55667788) return 3;
    if (w->tail != 0x12345678) return 4;

    struct S3 r3 = atomic_load(&w->a3);
    if (r3.b[0] != 0x22 || r3.b[1] != 0x33 || r3.b[2] != 0x44) return 5;
    struct S5 r5 = atomic_load(&w->a5);
    if (r5.b[0] != 1 || r5.b[4] != 5) return 6;
    struct S12 r12 = atomic_load(&w->a12);
    if (r12.b[0] != 10 || r12.b[11] != 21) return 7;

    atomic_store(&w->a3, ((struct S3){{ 0xAA, 0xBB, 0xCC }}));
    atomic_store(&w->a5, ((struct S5){{ 9, 10, 11, 12, 13 }}));
    w->tail = 0x76543210;
    return 0;
}
