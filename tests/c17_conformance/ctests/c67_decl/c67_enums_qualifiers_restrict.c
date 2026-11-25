/*
 * C99 §6.7.2.2 (enums), §6.7.3 (qualifiers on typedef arrays),
 * §6.7.3.1 (restrict), §6.7.4 (static inline & address-of inline).
 */
#include "c17_test.h"

enum Color {
    COL_NEG = -5,
    COL_NEXT,       /* -4 */
    COL_JUMP = 10,
    COL_AFTER,      /* 11 */
    COL_EXPR = COL_AFTER * 2 + COL_NEG, /* 17 */
}; /* trailing comma tested! */

typedef int Int3[3];

static inline int sq_inline(int x) {
    return x * x;
}

static void saxpy_restrict(int n, double a,
                           const double *restrict x,
                           double *restrict y) {
    for (int i = 0; i < n; i++) {
        y[i] += a * x[i];
    }
}

static int sum_static_param(int arr[static restrict 4]) {
    return arr[0] + arr[1] + arr[2] + arr[3];
}

int main(void) {
    C99_BEGIN("c67_enums_qualifiers_restrict", "Enums, typedef array const, restrict, static inline");

    /* Enum values and sizeof enumerator == sizeof(int) */
    C99_CHECK_EQ(COL_NEG, -5);
    C99_CHECK_EQ(COL_NEXT, -4);
    C99_CHECK_EQ(COL_AFTER, 11);
    C99_CHECK_EQ(COL_EXPR, 17);
    C99_CHECK_EQ(sizeof(COL_EXPR), sizeof(int));

    /* const applied to typedef of array applies to elements (§6.7.3 p8) */
    const Int3 ca = { 7, 8, 9 };
    const int *p = ca;
    C99_CHECK_EQ(p[0] + p[1] + p[2], 24);

    /* restrict pointers */
    double xs[4] = { 1.0, 2.0, 3.0, 4.0 };
    double ys[4] = { 10.0, 20.0, 30.0, 40.0 };
    saxpy_restrict(4, 2.5, xs, ys);
    C99_CHECK_DBL(ys[0], 12.5, 0.0);
    C99_CHECK_DBL(ys[3], 50.0, 0.0);

    int a4[4] = { 1, 2, 3, 4 };
    C99_CHECK_EQ(sum_static_param(a4), 10);

    /* static inline direct call and indirect call via function pointer */
    C99_CHECK_EQ(sq_inline(9), 81);
    int (*fp)(int) = sq_inline;
    C99_CHECK_EQ(fp(11), 121);

    /* volatile prevents elision of repeated writes/reads */
    volatile int v = 1;
    v = 2;
    v = v + 3;
    C99_CHECK_EQ(v, 5);

    C99_END();
}
