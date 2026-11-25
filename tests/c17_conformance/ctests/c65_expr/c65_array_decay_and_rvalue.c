/*
 * C99 §6.3.2.1 p3 & §6.5.15 p6: Array decay exceptions (sizeof, &, char[] init),
 * null pointer constants in ternary ?:, and struct rvalue member access.
 */
#include "c17_test.h"
#include <stddef.h>

struct Inner { int a; int b; };
struct Outer { struct Inner in; int tag; };

static struct Outer make_outer(int x) {
    struct Outer o = { { x, x * 2 }, x + 100 };
    return o;
}

int main(void) {
    C99_BEGIN("c65_array_decay_and_rvalue", "Array decay exceptions, null ptr ternary, struct rvalue member");

    /* Array decay exceptions: sizeof(arr) and &arr */
    int arr[5] = { 10, 20, 30, 40, 50 };
    C99_CHECK_EQ(sizeof(arr), 5 * sizeof(int));
    int (*p_arr)[5] = &arr;
    /* &arr + 1 advances by 5 * sizeof(int) bytes! */
    C99_CHECK_EQ((char *)(p_arr + 1) - (char *)p_arr, (ptrdiff_t)(5 * sizeof(int)));
    C99_CHECK_EQ((*p_arr)[3], 40);

    /* Null pointer constants in ternary: 0, 1-1, (void*)0 */
    int x = 55;
    int *p1 = c99_opaque_int(1) ? &x : 0;
    int *p2 = c99_opaque_int(0) ? &x : (1 - 1);
    int *p3 = c99_opaque_int(1) ? &x : (void *)(2 - 2);
    C99_CHECK_EQ(*p1, 55);
    C99_CHECK(p2 == NULL);
    C99_CHECK_EQ(*p3, 55);

    /* Accessing members of a struct rvalue returned by a function call */
    C99_CHECK_EQ(make_outer(7).in.a, 7);
    C99_CHECK_EQ(make_outer(7).in.b, 14);
    C99_CHECK_EQ(make_outer(7).tag, 107);

    C99_END();
}
