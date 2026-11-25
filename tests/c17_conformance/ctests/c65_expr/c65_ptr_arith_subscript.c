/*
 * C99 §6.5.2.1 & §6.5.6: Pointer arithmetic, commutative subscripting (i[a]),
 * negative indexing, one-past-the-end pointers, and ptrdiff_t on non-power-of-2
 * struct sizes (3B, 5B, 12B, 24B, 37B).
 */
#include "c17_test.h"
#include <stddef.h>

struct S3  { char b[3]; };
struct S5  { char b[5]; };
struct S12 { int a, b, c; };
struct S24 { long long a, b, c; };
struct S37 { char b[37]; };

int main(void) {
    C99_BEGIN("c65_ptr_arith_subscript", "Pointer arithmetic, i[a], negative idx, non-pow2 ptrdiff");

    int arr[6] = { 10, 20, 30, 40, 50, 60 };

    /* Commutative subscripting: i[arr] == arr[i] */
    C99_CHECK_EQ(0[arr], 10);
    C99_CHECK_EQ(3[arr], 40);
    C99_CHECK_EQ(5[arr], 60);

    /* Negative subscripting from middle pointer */
    int *mid = &arr[3];
    C99_CHECK_EQ(mid[-3], 10);
    C99_CHECK_EQ(mid[-1], 30);
    C99_CHECK_EQ((-2)[mid], 20);
    C99_CHECK_EQ(mid[2], 60);

    /* One-past-the-end pointer arithmetic & comparison */
    int *end = &arr[6];
    C99_CHECK(end == arr + 6);
    C99_CHECK(end > mid);
    C99_CHECK_EQ(end - arr, 6);
    C99_CHECK_EQ(arr - end, -6);

    /* Pointer subtraction on non-power-of-2 struct sizes! */
    struct S3  a3[10];
    struct S5  a5[10];
    struct S12 a12[10];
    struct S24 a24[10];
    struct S37 a37[10];

    C99_CHECK_EQ(&a3[7] - &a3[2], 5);
    C99_CHECK_EQ(&a3[1] - &a3[9], -8);
    C99_CHECK_EQ(&a5[8] - &a5[1], 7);
    C99_CHECK_EQ(&a5[2] - &a5[7], -5);
    C99_CHECK_EQ(&a12[9] - &a12[3], 6);
    C99_CHECK_EQ(&a12[0] - &a12[10], -10);
    C99_CHECK_EQ(&a24[6] - &a24[1], 5);
    C99_CHECK_EQ(&a37[9] - &a37[2], 7);
    C99_CHECK_EQ(&a37[2] - &a37[9], -7);

    /* Multi-dimensional array pointer arithmetic */
    int mat[3][4] = {
        { 1, 2, 3, 4 },
        { 5, 6, 7, 8 },
        { 9, 10, 11, 12 }
    };
    int (*rp)[4] = mat;
    C99_CHECK_EQ((*(rp + 2))[3], 12);
    C99_CHECK_EQ((rp + 3) - rp, 3);
    C99_CHECK_EQ(rp[1][2], 7);

    C99_END();
}
