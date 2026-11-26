#ifndef R6_ATOMIC_INTEROP_H
#define R6_ATOMIC_INTEROP_H
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

struct S3 { uint8_t b[3]; };
struct S5 { uint8_t b[5]; };
struct S12 { uint8_t b[12]; };

struct WrapAll {
    uint8_t tag;
    _Atomic struct S3 a3;
    int32_t mid1;
    _Atomic struct S5 a5;
    int32_t mid2;
    _Atomic struct S12 a12;
    int32_t tail;
};

size_t clang_sizeof_wrap(void);
size_t clang_offsetof_tail(void);
int32_t clang_verify_and_mutate(struct WrapAll *w);
#endif
