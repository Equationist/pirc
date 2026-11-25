/*
 * C99 §6.7.5: Complex declarators, abstract declarators, qualified array
 * parameters ([static const restrict volatile N]), and VLA [*] prototypes.
 */
#include <stddef.h>

/* Array parameter qualifiers (C99 §6.7.5.3 p7) */
int f_static_arr(int a[static 10]);
int f_qual_arr(int a[const volatile restrict static 4]);
int f_qual_before_static(int a[restrict static 4]);
int f_static_before_qual(int a[static const 4]);
int f_unbounded_qual(int a[restrict]);

/* VLA prototype with [*] (C99 §6.7.5.2 p4) */
int f_vla_star(int, int, int[*][*]);
int f_vla_star_qual(int, int[const restrict *]);

/* Function taking function (adjusts to pointer) and returning fn ptr */
typedef int (*binop_t)(int, int);
int (*select_op(int which, int f(int, int), int g(int, int)))(int, int);

/* Function returning pointer to array of 4 pointers to functions returning int */
int (*(*complex_ret(int x))[4])(double);

/* Abstract declarators in sizeof */
size_t abstract_sizes(void) {
    size_t s = 0;
    s += sizeof(int (*)[4]);
    s += sizeof(int (*(*)(int))[4]);
    s += sizeof(void (*(*[3])(void (*)(int)))(int));
    s += sizeof(const volatile int *restrict);
    return s;
}

int f_static_arr(int a[static 10]) {
    return a[0] + a[9];
}

int f_qual_arr(int a[const volatile restrict static 4]) {
    return a[0] + a[3];
}

int f_vla_star(int n, int m, int a[n][m]) {
    return a[n - 1][m - 1];
}

int (*select_op(int which, int f(int, int), int g(int, int)))(int, int) {
    return which ? f : g;
}
