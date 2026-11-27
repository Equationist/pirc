/* C17 §6.7 / §6.5.1.1: Round 9 declaration, inline, _Alignas, and _Generic checks */
#include "c17_test.h"
#include <stdalign.h>
#include <stdint.h>

/* 1. Prototype VLA [n] and [*] compatibility */
int vla_proto(int n, int a[n]);
int vla_proto(int n, int a[*]);
int vla_proto(int n, int a[n]) { return a[n - 1]; }

/* 2. _Atomic array parameter qualifier symmetry */
int atomic_arr_param(int a[_Atomic 3]);
int atomic_arr_param(int a[_Atomic 3]) { return a[0] + a[1] + a[2]; }

/* 3. _Alignas after tentative definition */
int tent_aligned;
_Alignas(16) int tent_aligned = 77;

/* 4. static const array inside inline function (C17 §6.7.4p3) */
inline int inline_const_arr(int i) {
    static const int tbl[3] = { 10, 20, 30 };
    return tbl[i];
}
extern inline int inline_const_arr(int i);

/* 5. extern inline function may define non-const static local */
extern inline int ext_inline_counter(void) {
    static int c = 0;
    return ++c;
}

/* 6. Function declared non-inline before inline definition has external definition */
int prior_decl_inline(void);
inline int prior_decl_inline(void) {
    static int x = 5;
    return ++x;
}

int main(void) {
    int arr[3] = { 3, 4, 5 };
    C17_CHECK(vla_proto(3, arr) == 5);
    C17_CHECK(atomic_arr_param(arr) == 12);
    C17_CHECK(tent_aligned == 77);
    C17_CHECK(((uintptr_t)&tent_aligned % 16) == 0);
    C17_CHECK(inline_const_arr(2) == 30);
    C17_CHECK(ext_inline_counter() == 1);
    C17_CHECK(ext_inline_counter() == 2);
    C17_CHECK(prior_decl_inline() == 6);

    /* 7. Unselected _Generic branch with compound literal must not be evaluated */
    int side = 0;
    int g = _Generic(1,
        int: 42,
        double: ((struct { int a; }){ ++side }).a
    );
    C17_CHECK(g == 42 && side == 0);

    C17_END();
}
