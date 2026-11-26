/* C17 §6.5.16.1p1 & §7.17:
 * - Nested _Atomic struct member initialization & _Atomic struct array init
 * - void * <-> _Atomic T * implicit conversions
 * - 8B & 16B _Atomic struct alignment & load/store */
#include "c17_test.h"
#include <stdatomic.h>
#include <stdalign.h>

struct Inner {
    int x;
    int y;
};

struct Outer {
    _Atomic struct Inner in;
    int tag;
};

struct Bytes8 {
    unsigned char b[8];
};

struct Big16 {
    long long a;
    long long b;
};

int main(void) {
    /* 1. Nested _Atomic struct initialization */
    struct Outer o = { (struct Inner){10, 20}, 30 };
    struct Inner got = atomic_load(&o.in);
    C17_CHECK(got.x == 10 && got.y == 20 && o.tag == 30);

    /* 2. Array of _Atomic structs initialization */
    _Atomic struct Inner arr[2] = { (struct Inner){1, 2}, (struct Inner){3, 4} };
    struct Inner a0 = atomic_load(&arr[0]);
    struct Inner a1 = atomic_load(&arr[1]);
    C17_CHECK(a0.x == 1 && a0.y == 2);
    C17_CHECK(a1.x == 3 && a1.y == 4);

    /* 3. void * <-> _Atomic int * conversions */
    _Atomic int ai = 42;
    void *vp = &ai;
    _Atomic int *ap = vp;
    C17_CHECK(atomic_load(ap) == 42);
    atomic_store(ap, 99);
    C17_CHECK(atomic_load(&ai) == 99);

    /* 4. 8-byte byte-array _Atomic struct alignment */
    C17_CHECK(alignof(_Atomic struct Bytes8) == 8);
    _Atomic struct Bytes8 af = (struct Bytes8){{1, 2, 3, 4, 5, 6, 7, 8}};
    struct Bytes8 fv = atomic_load(&af);
    C17_CHECK(fv.b[0] == 1 && fv.b[7] == 8);

    /* 5. 16-byte _Atomic struct */
    _Atomic struct Big16 b16 = (struct Big16){100, 200};
    C17_CHECK(alignof(_Atomic struct Big16) == 16);
    struct Big16 bv = atomic_load(&b16);
    C17_CHECK(bv.a == 100 && bv.b == 200);

    C17_END();
}
