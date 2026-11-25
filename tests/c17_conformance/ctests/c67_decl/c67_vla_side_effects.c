/*
 * C99 §6.7.5.2: VLA dimension side effects evaluated exactly once per
 * dimension, VLA decay to pointer argument, and VLA inside switch case block.
 */
#include "c17_test.h"

static int dim_evals = 0;

static int dim_fn(int v) {
    dim_evals++;
    return v;
}

static int sum_flat(const int *p, int count) {
    int s = 0;
    for (int i = 0; i < count; i++) {
        s += p[i];
    }
    return s;
}

static int vla_in_switch(int mode, int n) {
    switch (mode) {
    case 1: {
        int buf[n];
        for (int i = 0; i < n; i++) {
            buf[i] = i + 1;
        }
        return sum_flat(buf, n);
    }
    default:
        return -1;
    }
}

int main(void) {
    C99_BEGIN("c67_vla_side_effects", "VLA dimension side effects, pointer decay, VLA in switch block");

    dim_evals = 0;
    int grid[dim_fn(3)][dim_fn(4)];
    C99_CHECK_EQ(dim_evals, 2);
    C99_CHECK_EQ(sizeof(grid), 12 * sizeof(int));

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            grid[i][j] = i * 4 + j + 1;
        }
    }
    C99_CHECK_EQ(sum_flat(&grid[0][0], 12), 78);
    C99_CHECK_EQ(vla_in_switch(c99_opaque_int(1), c99_opaque_int(5)), 15);
    C99_CHECK_EQ(vla_in_switch(c99_opaque_int(0), c99_opaque_int(5)), -1);

    C99_END();
}
