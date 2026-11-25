/*
 * C99 §6.7.8: Designated initializers — out-of-order, nested, array length
 * deduction, overriding earlier initializers (scalar AND sub-aggregate reset!),
 * union non-first member init, brace elision, exact char[N] init, global relocs.
 */
#include "c17_test.h"

struct Inner {
    int x;
    int y;
};

struct Outer {
    struct Inner p;
    int arr[4];
    int tail;
};

union Tagged {
    int i;
    double d;
    struct Inner pt;
};

/* File-scope designated initializers */
static int g_arr[] = { [4] = 40, [1] = 10, 20 }; /* len 5: {0, 10, 20, 0, 40} */
static struct Outer g_out = {
    .tail = 99,
    .arr = { [2] = 22, [0] = 11 },
    .p = { .y = 8, .x = 7 },
};
static union Tagged g_u = { .d = 3.25 };
static union Tagged g_u2 = { .pt = { .y = 55, .x = 44 } };

/* Exact-length char array initializer (no NUL terminator, valid in C99 §6.7.8 p14) */
static const char exact3[3] = "abc";

/* Global pointer relocation with positive and negative addends */
static int base_vals[6] = { 100, 200, 300, 400, 500, 600 };
static int *p_plus = &base_vals[4];
static int *p_offset = base_vals + 2;

int main(void) {
    C99_BEGIN("c67_designated_init", "Designated initializers, overrides, unions, brace elision");

    /* Global array deduced length & values */
    C99_CHECK_EQ(sizeof(g_arr), 5 * sizeof(int));
    C99_CHECK_EQ(g_arr[0], 0);
    C99_CHECK_EQ(g_arr[1], 10);
    C99_CHECK_EQ(g_arr[2], 20);
    C99_CHECK_EQ(g_arr[3], 0);
    C99_CHECK_EQ(g_arr[4], 40);

    /* Global struct & union designated init */
    C99_CHECK_EQ(g_out.p.x, 7);
    C99_CHECK_EQ(g_out.p.y, 8);
    C99_CHECK_EQ(g_out.arr[0], 11);
    C99_CHECK_EQ(g_out.arr[1], 0);
    C99_CHECK_EQ(g_out.arr[2], 22);
    C99_CHECK_EQ(g_out.arr[3], 0);
    C99_CHECK_EQ(g_out.tail, 99);
    C99_CHECK_DBL(g_u.d, 3.25, 0.0);
    C99_CHECK_EQ(g_u2.pt.x, 44);
    C99_CHECK_EQ(g_u2.pt.y, 55);

    /* Exact char[3] = "abc" */
    C99_CHECK_EQ(sizeof(exact3), 3);
    C99_CHECK_EQ(exact3[0], 'a');
    C99_CHECK_EQ(exact3[2], 'c');

    /* Global pointer relocations */
    C99_CHECK_EQ(*p_plus, 500);
    C99_CHECK_EQ(*p_offset, 300);
    C99_CHECK_EQ(p_plus[-2], 300);

    /* Local designated initializers with runtime values & overrides (§6.7.8 p19) */
    int v = c99_opaque_int(5);
    /* Case 1: whole sub-aggregate first, then single sub-member override -> preserves other member! */
    struct Outer o1 = {
        .p = { v, v + 1 },
        .p.y = 77,
        .arr[3] = 33,
    };
    C99_CHECK_EQ(o1.p.x, 5);
    C99_CHECK_EQ(o1.p.y, 77);
    C99_CHECK_EQ(o1.arr[0], 0);
    C99_CHECK_EQ(o1.arr[3], 33);
    C99_CHECK_EQ(o1.tail, 0);

    /* Case 2: sub-member first, then whole sub-aggregate override -> resets unspecified sub-members to 0! */
    struct Outer o2 = {
        .p.x = 123,
        .p = { .y = 88 },
    };
    C99_CHECK_EQ(o2.p.x, 0);
    C99_CHECK_EQ(o2.p.y, 88);

    /* Case 3: array element override + subsequent sequential placement */
    int la[6] = { 1, 2, 3, [1] = 20, 30, [4] = 40 };
    /* [0]=1, [1]=20, [2]=30 (overwrote 3!), [3]=0, [4]=40, [5]=0 */
    C99_CHECK_EQ(la[0], 1);
    C99_CHECK_EQ(la[1], 20);
    C99_CHECK_EQ(la[2], 30);
    C99_CHECK_EQ(la[3], 0);
    C99_CHECK_EQ(la[4], 40);
    C99_CHECK_EQ(la[5], 0);

    /* Local union designated init (non-first member) */
    union Tagged lu = { .pt = { .x = v * 2, .y = v * 3 } };
    C99_CHECK_EQ(lu.pt.x, 10);
    C99_CHECK_EQ(lu.pt.y, 15);

    /* Brace elision in nested 2D array */
    int mat[2][3] = { 1, 2, 3, 4, 5 };
    C99_CHECK_EQ(mat[0][0], 1);
    C99_CHECK_EQ(mat[0][2], 3);
    C99_CHECK_EQ(mat[1][0], 4);
    C99_CHECK_EQ(mat[1][1], 5);
    C99_CHECK_EQ(mat[1][2], 0);

    C99_END();
}
