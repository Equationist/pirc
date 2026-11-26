#include "interop.h"
#include <stdarg.h>

struct AI1 clang_ai1(struct AI1 x) {
    struct AI1 r;
    atomic_init(&r.a, atomic_load(&x.a) + 1);
    return r;
}

struct AF2 clang_af2(struct AF2 x) {
    struct AF2 r;
    atomic_init(&r.a, atomic_load(&x.a) + 1.0f);
    atomic_init(&r.b, atomic_load(&x.b) + 2.0f);
    return r;
}

int clang_check_schar(signed char c) {
    return (int)c;
}

signed char clang_ret_schar(void) {
    return (signed char)-9;
}

int clang_va_pair(int a, ...) {
    va_list ap;
    va_start(ap, a);
    int b = va_arg(ap, int);
    va_end(ap);
    return a * 100 + b;
}
