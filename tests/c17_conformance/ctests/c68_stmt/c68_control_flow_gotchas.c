/*
 * C17 §6.8: Control-flow gotchas — sparse & negative-to-positive switches,
 * switch jumping into a loop body past non-VLA declarations, multi-level goto
 * escape, and deep recursion (500 frames).
 */
#include "c17_test.h"

static int sparse_switch(int x) {
    switch (x) {
    case -100000: return 1;
    case -1:      return 2;
    case 0:       return 3;
    case 1:       return 4;
    case 100000:  return 5;
    default:      return 0;
    }
}

/* Switch jumping into the middle of a loop past a non-VLA declaration */
static int switch_into_loop(int mode) {
    int sum = 0;
    switch (mode) {
        int uninit_skipped;
    case 0:
        for (int i = 0; i < 4; i++) {
            sum += 10;
    case 1:
            uninit_skipped = 5;
            sum += uninit_skipped;
        }
        break;
    default:
        sum = -1;
        break;
    }
    return sum;
}

static int deep_rec(int n) {
    int local[4] = { n, n + 1, n + 2, n + 3 };
    if (n <= 0) {
        return local[0];
    }
    return (local[3] - local[2]) + deep_rec(n - 1);
}

int main(void) {
    C17_BEGIN("c68_control_flow_gotchas", "Sparse switch, switch-into-loop, 4-level goto escape, deep recursion");

    C17_CHECK_EQ(sparse_switch(c17_opaque_int(-100000)), 1);
    C17_CHECK_EQ(sparse_switch(c17_opaque_int(-1)), 2);
    C17_CHECK_EQ(sparse_switch(c17_opaque_int(0)), 3);
    C17_CHECK_EQ(sparse_switch(c17_opaque_int(1)), 4);
    C17_CHECK_EQ(sparse_switch(c17_opaque_int(100000)), 5);
    C17_CHECK_EQ(sparse_switch(c17_opaque_int(7)), 0);

    /* mode=0 runs 4 full iterations: 4 * (10 + 5) = 60 */
    C17_CHECK_EQ(switch_into_loop(c17_opaque_int(0)), 60);

    /* 4-level nested loop escape via goto */
    int found = 0;
    for (int a = 0; a < 5; a++) {
        for (int b = 0; b < 5; b++) {
            for (int c = 0; c < 5; c++) {
                for (int d = 0; d < 5; d++) {
                    if (a == 2 && b == 3 && c == 1 && d == 4) {
                        found = a * 1000 + b * 100 + c * 10 + d;
                        goto escaped;
                    }
                }
            }
        }
    }
escaped:
    C17_CHECK_EQ(found, 2314);

    /* 500-frame recursion */
    C17_CHECK_EQ(deep_rec(c17_opaque_int(500)), 500);

    C17_END();
}
