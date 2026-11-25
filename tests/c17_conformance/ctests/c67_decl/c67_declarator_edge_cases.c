/*
 * C17 §6.7.6 & §6.7.8: Declarator edge cases — redundant parens around
 * declarators, typedef-in-parameter disambiguation (int f(int (T))),
 * array/function parameter decay, and const + pointer typedef gotcha!
 */
#include "c17_test.h"

typedef int MyInt;
typedef int *IntPtr;

/* Classic parser disambiguation (§6.7.6.3 p11):
 * Because MyInt is a typedef name, 'int (MyInt)' is a parameter of function
 * type (adjusted to function pointer 'int (*)(MyInt)'), NOT a parameter named
 * 'MyInt' of type 'int'! */
static int call_disambig(int (MyInt));

static int call_disambig(int (*fn)(MyInt)) {
    return fn(21);
}

static int dbl_it(int x) { return x * 2; }

/* Redundant parentheses around declarator names */
static int (paren_fn)(int (a), int ((b))) {
    int (local) = a + b;
    return local;
}

/* Multidimensional array param decay: int a[][3] -> int (*a)[3] */
static int sum_rows(int a[][3], int nrows) {
    int s = 0;
    for (int i = 0; i < nrows; i++) {
        for (int j = 0; j < 3; j++) {
            s += a[i][j];
        }
    }
    return s;
}

int main(void) {
    C17_BEGIN("c67_declarator_edge_cases", "Typedef param disambiguation, paren declarators, const IntPtr");

    C17_CHECK_EQ(call_disambig(dbl_it), 42);
    C17_CHECK_EQ(paren_fn(10, 32), 42);

    int (*((fp)))(int) = dbl_it;
    C17_CHECK_EQ(fp(9), 18);

    int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    C17_CHECK_EQ(sum_rows(m, 2), 21);

    /* Classic typedef + const gotcha:
     * 'const IntPtr p' means 'int *const p' (const pointer to MUTABLE int),
     * NOT 'const int *p'! Modifying *p MUST be legal! */
    int target = 10;
    const IntPtr p = &target;
    *p = 99;
    C17_CHECK_EQ(target, 99);
    C17_CHECK_EQ(_Generic(p, int *: 1, const int *: 2, default: 0), 1);

    C17_END();
}
