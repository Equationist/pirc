/*
 * C99 §6.5 & §6.8: Optimizer & register-allocation stress via valid C99 —
 * cyclic loop-carried permutations (2/3/5-cycles), address-taken locals,
 * struct member pointer aliasing, and deep control-flow diamonds.
 */
#include "c17_test.h"

struct Pair {
    int first;
    int second;
};

static int rotate5(int n) {
    int a = 1, b = 2, c = 3, d = 4, e = 5;
    for (int i = 0; i < n; i++) {
        int tmp = a;
        a = b;
        b = c;
        c = d;
        d = e;
        e = tmp;
    }
    return a * 10000 + b * 1000 + c * 100 + d * 10 + e;
}

static int swap_diamond(int n) {
    int x = 10, y = 20;
    for (int i = 0; i < n; i++) {
        if ((i & 1) == 0) {
            int t = x; x = y; y = t;
        } else {
            x += 1;
            y += 2;
        }
    }
    return x * 100 + y;
}

static void mutate_via_ptr(int *p) {
    *p += 37;
}

int main(void) {
    C99_BEGIN("c65_swap_phi_cycles", "Cyclic permutations, address-taken locals, struct member aliasing");

    /* 5-way rotation: after 2 steps -> 34512; after 5 steps -> 12345 */
    C99_CHECK_EQ(rotate5(c99_opaque_int(2)), 34512);
    C99_CHECK_EQ(rotate5(c99_opaque_int(5)), 12345);
    C99_CHECK_EQ(rotate5(c99_opaque_int(13)), 45123);

    /* Diamond swap:
     * i=0 (even): (20, 10)
     * i=1 (odd):  (21, 12)
     * i=2 (even): (12, 21)
     * i=3 (odd):  (13, 23) */
    C99_CHECK_EQ(swap_diamond(c99_opaque_int(4)), 1323);

    /* Address-taken local modified through pointer */
    int loc = 5;
    mutate_via_ptr(&loc);
    C99_CHECK_EQ(loc, 42);

    /* Struct member pointer aliasing */
    struct Pair pr = { 10, 20 };
    int *p_second = &pr.second;
    *p_second = 99;
    C99_CHECK_EQ(pr.first, 10);
    C99_CHECK_EQ(pr.second, 99);

    /* Conditional pointer aliasing */
    int u = 1, v = 2;
    int *chosen = c99_opaque_int(1) ? &u : &v;
    *chosen += 50;
    C99_CHECK_EQ(u, 51);
    C99_CHECK_EQ(v, 2);

    C99_END();
}
