#ifndef R7_ATOMIC_CALL_CAST_INTEROP_H
#define R7_ATOMIC_CALL_CAST_INTEROP_H
#include <stdatomic.h>

struct AI1 {
    _Atomic int a;
};

struct AF2 {
    _Atomic float a;
    _Atomic float b;
};

struct AI1 clang_ai1(struct AI1 x);
struct AF2 clang_af2(struct AF2 x);

int clang_check_schar(signed char c);
signed char clang_ret_schar(void);
int clang_va_pair(int a, ...);
#endif
