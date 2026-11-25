/*
 * C99 §6.5.3.4 p2: sizeof operand is NOT evaluated UNLESS its type is a VLA!
 */
#include "c17_test.h"

static int side_calls = 0;

static int bump(int val) {
    side_calls++;
    return val;
}

int main(void) {
    C99_BEGIN("c65_sizeof_vla_eval", "sizeof unevaluated ordinary expr vs evaluated VLA type");

    int x = 10;
    /* Ordinary expressions: MUST NOT be evaluated */
    size_t s1 = sizeof(++x);
    size_t s2 = sizeof(bump(5));
    size_t s3 = sizeof(*(int *)0);
    C99_CHECK_EQ(s1, sizeof(int));
    C99_CHECK_EQ(s2, sizeof(int));
    C99_CHECK_EQ(s3, sizeof(int));
    C99_CHECK_EQ(x, 10);
    C99_CHECK_EQ(side_calls, 0);

    /* VLA type operand: MUST be evaluated at runtime! */
    int n = c99_opaque_int(4);
    size_t sv1 = sizeof(int[++n]);
    C99_CHECK_EQ(n, 5);
    C99_CHECK_EQ(sv1, 5 * sizeof(int));

    /* 2D VLA type operand: both dimension expressions evaluated */
    size_t sv2 = sizeof(short[bump(3)][bump(7)]);
    C99_CHECK_EQ(side_calls, 2);
    C99_CHECK_EQ(sv2, 21 * sizeof(short));

    /* sizeof on an actual VLA object */
    int m = c99_opaque_int(6);
    double vla[m];
    C99_CHECK_EQ(sizeof(vla), 6 * sizeof(double));
    /* Modifying m AFTER declaration does NOT change sizeof(vla)! */
    m = 100;
    C99_CHECK_EQ(sizeof(vla), 6 * sizeof(double));

    /* sizeof on pointer to VLA is compile-time sizeof(void*), does NOT need
     * runtime size, wait — per §6.5.3.4 p2, int (*)[bump(4)] is a VM type
     * (pointer to VLA, not itself a VLA) so sizeof(int(*)[...]) is unspecified
     * whether inner expr is evaluated; stick to guaranteed VLA sizeof above. */

    /* sizeof on string literal includes NUL terminator */
    C99_CHECK_EQ(sizeof("hello"), 6);
    C99_CHECK_EQ(sizeof(""), 1);

    C99_END();
}
