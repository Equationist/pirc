/* Bidirectional clang <-> slimcc-pir Round 7 ABI interop:
 * - Structs containing _Atomic scalar/float fields (non-HFA on ARM64, MEMORY on SysV x86-64)
 * - Function-pointer casts across signext/zeroext & variadic signatures */
#include "c17_test.h"
#include "interop.h"

int main(void) {
    struct AF2 a;
    atomic_init(&a.a, 1.5f);
    atomic_init(&a.b, 2.5f);
    struct AF2 ar = clang_af2(a);
    C17_CHECK(atomic_load(&ar.a) == 2.5f);
    C17_CHECK(atomic_load(&ar.b) == 4.5f);

    struct AI1 i;
    atomic_init(&i.a, 41);
    struct AI1 ir = clang_ai1(i);
    C17_CHECK_EQ(atomic_load(&ir.a), 42);

    /* Cast through generic function pointer back to exact ABI signature */
    void (*p1)(void) = (void (*)(void))clang_check_schar;
    void (*p2)(void) = (void (*)(void))clang_ret_schar;
    void (*p3)(void) = (void (*)(void))clang_va_pair;

    C17_CHECK_EQ(((int (*)(signed char))p1)((signed char)-7), -7);
    C17_CHECK_EQ((int)((signed char (*)(void))p2)(), -9);
    C17_CHECK_EQ(((int (*)(int, ...))p3)(12, 34), 1234);

    C17_END();
}
