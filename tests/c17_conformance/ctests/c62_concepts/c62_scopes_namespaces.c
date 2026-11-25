/*
 * C99 §6.2.1, §6.2.2, §6.2.3, §6.7.2.3, §6.8.4, §6.8.5:
 * Scopes, 4 namespaces, tag shadowing via inner forward decl, C99 selection/
 * iteration scopes, static local persistence, and linkage inheritance.
 */
#include "c17_test.h"

/* §6.2.2 p4: prior static declaration gives subsequent non-static decl internal linkage */
static int internal_fn(int x);
int internal_fn(int x) {
    return x * 3;
}

/* Outer struct S */
struct S {
    int a;
    int b;
};

static int test_tag_shadow(void) {
    struct S outer = { 1, 2 };
    int outer_sz = (int)sizeof(struct S);
    int inner_sz = 0;
    {
        /* §6.7.2.3 p5: "struct S;" declares a NEW incomplete tag in inner scope! */
        struct S;
        struct S {
            long long x;
            long long y;
            long long z;
        };
        struct S inner = { 10, 20, 30 };
        inner_sz = (int)sizeof(struct S);
        if (inner.x + inner.y + inner.z != 60) {
            return -1;
        }
    }
    /* Outer struct S is restored */
    if ((int)sizeof(struct S) != outer_sz) {
        return -2;
    }
    if (inner_sz <= outer_sz) {
        return -3;
    }
    return outer.a + outer.b;
}

/* All 4 namespaces with the same identifier 'foo' */
static int test_four_namespaces(void) {
    struct foo {
        int foo;
    };
    struct foo foo = { 42 };
    int sum = 0;
foo:
    sum += foo.foo;
    if (sum < 84) {
        goto foo;
    }
    return sum;
}

/* §6.8.4 p3 & §6.8.5 p5: C99 block scopes for selection and iteration statements */
static int test_c99_stmt_scopes(void) {
    enum { K = 100 };
    int seen_if = 0;
    int seen_after = 0;

    /* In C99, the controlling expression of 'if' is inside the if-statement's block scope,
     * and the substatement is its own inner block scope! */
    if (sizeof(enum { K = 7 }) > 0) {
        seen_if = K; /* Must be 7 */
    }
    seen_after = K; /* Must be 100 again! */

    /* Same for while/for/switch */
    int seen_sw = 0;
    switch ((int)sizeof(enum { K = 23 })) {
    default:
        seen_sw = K; /* Must be 23 */
        break;
    }

    if (seen_if != 7 || seen_after != 100 || seen_sw != 23) {
        return -1;
    }
    return K;
}

/* Static local persistence across recursive calls */
static int rec_counter(int n) {
    static int total = 0;
    total++;
    if (n > 0) {
        return rec_counter(n - 1);
    }
    return total;
}

int main(void) {
    C99_BEGIN("c62_scopes_namespaces", "Scopes, 4 namespaces, tag shadowing, C99 stmt scopes");
    C99_CHECK_EQ(internal_fn(7), 21);
    C99_CHECK_EQ(test_tag_shadow(), 3);
    C99_CHECK_EQ(test_four_namespaces(), 84);
    C99_CHECK_EQ(test_c99_stmt_scopes(), 100);
    C99_CHECK_EQ(rec_counter(4), 5);
    C99_CHECK_EQ(rec_counter(2), 8);
    C99_END();
}
