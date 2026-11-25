/*
 * C99 §6.5.2.2 & §6.5.3.2 p4: Function pointers, arbitrary dereference chains
 * (****fp)(), dispatch tables, mutual recursion, and libc qsort callback.
 */
#include "c17_test.h"
#include <stdlib.h>

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }

static int is_odd(int n);
static int is_even(int n) {
    return (n == 0) ? 1 : is_odd(n - 1);
}
static int is_odd(int n) {
    return (n == 0) ? 0 : is_even(n - 1);
}

static int cmp_ints(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

int main(void) {
    C99_BEGIN("c65_fnptr_recursion_callbacks", "Function pointers, (****fp)(), mutual recursion, qsort");

    /* Function designator & arbitrary dereference chain (§6.5.3.2 p4) */
    int (*fp)(int, int) = op_add;
    C99_CHECK_EQ(fp(3, 4), 7);
    C99_CHECK_EQ((*fp)(3, 4), 7);
    C99_CHECK_EQ((****fp)(3, 4), 7);
    C99_CHECK_EQ((***op_mul)(6, 7), 42);
    C99_CHECK(fp == &op_add);
    C99_CHECK(fp == *fp);

    /* Dispatch table */
    int (*ops[3])(int, int) = { op_add, op_sub, op_mul };
    C99_CHECK_EQ(ops[0](10, 3), 13);
    C99_CHECK_EQ(ops[1](10, 3), 7);
    C99_CHECK_EQ(ops[2](10, 3), 30);

    /* Mutual recursion (120 frames deep) */
    C99_CHECK_EQ(is_even(c99_opaque_int(120)), 1);
    C99_CHECK_EQ(is_odd(c99_opaque_int(121)), 1);
    C99_CHECK_EQ(is_even(c99_opaque_int(121)), 0);

    /* libc qsort calling back into compiled comparator */
    int arr[8] = { 42, -5, 17, 0, 99, -100, 17, 3 };
    qsort(arr, 8, sizeof(arr[0]), cmp_ints);
    C99_CHECK_EQ(arr[0], -100);
    C99_CHECK_EQ(arr[1], -5);
    C99_CHECK_EQ(arr[2], 0);
    C99_CHECK_EQ(arr[3], 3);
    C99_CHECK_EQ(arr[4], 17);
    C99_CHECK_EQ(arr[5], 17);
    C99_CHECK_EQ(arr[6], 42);
    C99_CHECK_EQ(arr[7], 99);

    C99_END();
}
