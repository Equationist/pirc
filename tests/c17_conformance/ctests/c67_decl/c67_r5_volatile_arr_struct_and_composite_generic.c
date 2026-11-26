/* C17 §6.5.2.3, §6.5.3.2p3, §6.5.1.1, §6.7.3:
 * - volatile struct member qualifier propagation & volatile array initialization
 * - volatile struct assignment must not re-read LHS
 * - composite_type on qualified pointer-to-array preserves unqual() for _Generic
 * - &*vp on void * and const void * */
#include "c17_test.h"

struct Pair {
    int a;
    int b;
};

static int target_arr[3] = {10, 20, 30};
extern int (*const comp_p)[];
extern int (*const comp_p)[3];
int (*const comp_p)[3] = &target_arr;

int main(void) {
    /* 1. _Generic on composite qualified pointer-to-array */
    int g = _Generic(comp_p, int (*)[3]: 1, default: 0);
    C17_CHECK(g == 1);
    C17_CHECK((*comp_p)[1] == 20);

    /* 2. volatile struct member qualifier propagation and assignment */
    volatile struct Pair vp = {11, 22};
    C17_CHECK(vp.a == 11);
    C17_CHECK(vp.b == 22);
    struct Pair src = {33, 44};
    struct Pair res = (vp = src);
    C17_CHECK(res.a == 33 && res.b == 44);
    C17_CHECK(vp.a == 33 && vp.b == 44);

    /* 3. volatile array initialization */
    volatile int va[4] = {5, 6, 7, 8};
    C17_CHECK(va[0] == 5 && va[3] == 8);

    /* 4. &*vp on void * and const void * (C17 §6.5.3.2p3) */
    int x = 99;
    void *p = &x;
    const void *cp = &x;
    void *p2 = &*p;
    const void *cp2 = &*cp;
    C17_CHECK(p2 == p);
    C17_CHECK(cp2 == cp);
    C17_CHECK(*(int *)p2 == 99);
    C17_CHECK(*(const int *)cp2 == 99);

    C17_END();
}
