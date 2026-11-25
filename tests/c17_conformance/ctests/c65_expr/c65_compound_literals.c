/*
 * C99 §6.5.2.5: Compound literals — lvalue semantics, static vs automatic
 * storage duration, loop re-initialization & enclosing-block lifetime,
 * deduced array sizes, designated initializers, and nested compound literals.
 */
#include "c17_test.h"

struct Point {
    int x;
    int y;
};

struct Line {
    struct Point a;
    struct Point b;
};

/* File-scope compound literal: static storage duration */
static int *global_cl_ptr = (int[]){ 10, 20, 30 };
static struct Point *global_pt = &(struct Point){ .y = 99, .x = 11 };

static int *get_static_cl(void) {
    return global_cl_ptr;
}

static int sum_point(struct Point p) {
    return p.x * 10 + p.y;
}

static int sum_array(const int *a, int n) {
    int s = 0;
    for (int i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

/* §6.5.2.5 p6: compound literal inside a block survives until end of enclosing block */
static int test_loop_lifetime(void) {
    int sum = 0;
    int *ptrs[3];
    for (int i = 0; i < 3; i++) {
        /* Re-initialized to (i + 1) on every iteration */
        int *p = &(int){ i + 1 };
        *p *= 10;
        sum += *p;
        ptrs[i] = p;
    }
    (void)ptrs;
    /* 10 + 20 + 30 = 60 */
    return sum;
}

/* Compound literal modified in same block across a backward goto */
static int test_goto_same_block(void) {
    int iter = 0;
    int *p = NULL;
again:
    if (iter == 0) {
        p = &(int){ 5 };
    } else {
        /* Same enclosing block was NOT exited, so *p must still be valid! */
        *p += 7;
    }
    iter++;
    if (iter < 3) {
        goto again;
    }
    return *p; /* 5 + 7 + 7 = 19 */
}

int main(void) {
    C99_BEGIN("c65_compound_literals", "Compound literals: lvalues, lifetimes, arrays, structs");

    /* File-scope static compound literals */
    C99_CHECK_EQ(global_cl_ptr[0], 10);
    C99_CHECK_EQ(global_cl_ptr[2], 30);
    global_cl_ptr[1] += 5;
    C99_CHECK_EQ(get_static_cl()[1], 25);
    C99_CHECK_EQ(global_pt->x, 11);
    C99_CHECK_EQ(global_pt->y, 99);

    /* Compound literal as modifiable lvalue */
    int v = ++(int){ 41 };
    C99_CHECK_EQ(v, 42);
    int w = ((int){ 10 } = 55);
    C99_CHECK_EQ(w, 55);

    /* Array compound literal with omitted size */
    C99_CHECK_EQ(sizeof((int[]){ 1, 2, 3, 4, 5 }), 5 * sizeof(int));
    C99_CHECK_EQ(sum_array((int[]){ 2, 4, 6, 8 }, 4), 20);

    /* Nested struct compound literal with designated initializers */
    struct Line ln = (struct Line){
        .b = (struct Point){ .y = 8, .x = 7 },
        .a = { .x = 1, .y = 2 },
    };
    C99_CHECK_EQ(ln.a.x, 1);
    C99_CHECK_EQ(ln.a.y, 2);
    C99_CHECK_EQ(ln.b.x, 7);
    C99_CHECK_EQ(ln.b.y, 8);

    /* Passing compound literal by value */
    C99_CHECK_EQ(sum_point((struct Point){ .y = 3, .x = 4 }), 43);

    /* Loop re-initialization and enclosing-block lifetime */
    C99_CHECK_EQ(test_loop_lifetime(), 60);
    C99_CHECK_EQ(test_goto_same_block(), 19);

    C99_END();
}
