/*
 * C99 §6.8.4 & §6.8.5: Dangling else binding, controlling expressions of
 * pointer/float/_Bool/bitfield types, and do { ... } while (0) idioms.
 */
#include "c17_test.h"

struct Flag {
    unsigned int bit : 1;
};

#define INC_TWICE(x) do { (x) += 1; (x) += 1; } while (0)

static int dangling_else(int a, int b) {
    int r = 0;
    if (a)
        if (b)
            r = 1;
        else
            r = 2; /* Must bind to inner if(b), NOT outer if(a)! */
    return r;
}

int main(void) {
    C99_BEGIN("c68_dangling_else_ctrl", "Dangling else, pointer/float/bitfield conditions, do-while(0)");

    C99_CHECK_EQ(dangling_else(0, 0), 0);
    C99_CHECK_EQ(dangling_else(0, 1), 0);
    C99_CHECK_EQ(dangling_else(1, 0), 2);
    C99_CHECK_EQ(dangling_else(1, 1), 1);

    /* Controlling expression of float type (0.1 is truthy, -0.0 is falsy!) */
    int f_taken = 0;
    if (c99_opaque_dbl(0.1)) {
        f_taken += 1;
    }
    if (-c99_opaque_dbl(0.0)) {
        f_taken += 100;
    }
    C99_CHECK_EQ(f_taken, 1);

    /* Controlling expression of pointer and bitfield types */
    int target = 5;
    int *p = &target;
    int steps = 0;
    while (p) {
        steps++;
        p = NULL;
    }
    C99_CHECK_EQ(steps, 1);

    struct Flag fl = { 1u };
    int bf_ok = 0;
    if (fl.bit) {
        bf_ok = 1;
    }
    C99_CHECK_EQ(bf_ok, 1);

    /* do-while(0) macro inside unbraced if-else */
    int v = 10;
    if (c99_opaque_int(1))
        INC_TWICE(v);
    else
        v = -1;
    C99_CHECK_EQ(v, 12);

    C99_END();
}
