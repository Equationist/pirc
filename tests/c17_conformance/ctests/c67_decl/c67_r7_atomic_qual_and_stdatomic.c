/* C17 §6.7.2.4, §7.17:
 * - Qualified (_Atomic + const/volatile) non-power-of-two structs preserve size/alignment
 * - atomic_load() returns unqualified non-atomic value type (sizeof & variadic ABI)
 * - atomic_init / atomic_store / atomic_flag_clear return void
 * - atomic_is_lock_free returns 0 for large >8B _Atomic structs */
#include "c17_test.h"
#include <stdalign.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>

struct S3 {
    unsigned char b[3];
};

struct S12 {
    int a;
    int b;
    int c;
};

struct S32 {
    unsigned char b[32];
};

typedef _Atomic struct S3 AS3;

static int va_check_s12(int n, ...) {
    va_list ap;
    va_start(ap, n);
    int x = va_arg(ap, int);
    struct S12 s = va_arg(ap, struct S12);
    int tail = va_arg(ap, int);
    va_end(ap);
    return (x == 42 && s.a == 111 && s.b == 222 && s.c == 333 && tail == 999);
}

int main(void) {
    /* 1. const/volatile qualified _Atomic non-pow2 struct preserves size & align */
    C17_CHECK_EQ(sizeof(const AS3), sizeof(AS3));
    C17_CHECK_EQ(sizeof(volatile AS3), sizeof(AS3));
    C17_CHECK_EQ(alignof(volatile AS3), alignof(AS3));
    volatile AS3 arr[2];
    C17_CHECK_EQ(sizeof(arr), 2u * sizeof(AS3));
    C17_CHECK(((uintptr_t)&arr[1] % alignof(AS3)) == 0);

    AS3 g3;
    struct S3 init3 = { { 7, 7, 7 } };
    atomic_init(&g3, init3);
    const AS3 *cp = &g3;
    struct S3 r3 = atomic_load(cp);
    C17_CHECK(r3.b[0] == 7 && r3.b[1] == 7 && r3.b[2] == 7);

    /* 2. atomic_load unqualified type & variadic forwarding */
    _Atomic struct S12 a12;
    struct S12 v12 = { 111, 222, 333 };
    atomic_init(&a12, v12);
    C17_CHECK_EQ(sizeof(atomic_load(&a12)), sizeof(struct S12));
    C17_CHECK(va_check_s12(3, 42, atomic_load(&a12), 999));

    /* 3. void return types for atomic_init / atomic_store / atomic_flag_clear */
    _Atomic int ai = 0;
    atomic_flag fl = ATOMIC_FLAG_INIT;
    int cond = c17_opaque_int(1);
    cond ? atomic_init(&ai, 10) : (void)0;
    cond ? atomic_store(&ai, 25) : (void)0;
    cond ? atomic_flag_clear(&fl) : (void)0;
    C17_CHECK_EQ(atomic_load(&ai), 25);
    C17_CHECK(!atomic_flag_test_and_set(&fl));

    /* 4. Large 32-byte _Atomic struct is not lock-free */
    _Atomic struct S32 a32;
    C17_CHECK(!atomic_is_lock_free(&a32));

    C17_END();
}
