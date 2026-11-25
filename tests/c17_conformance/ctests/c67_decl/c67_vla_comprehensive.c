/*
 * C99 §6.7.5.2 & §6.7.7: Variable Length Arrays — 1D/2D/3D VLAs, pointers to
 * VLAs, VLA parameters with [*] prototypes, typedef VLA snapshot semantics,
 * loop stack deallocation, and goto exiting VLA scope.
 */
#include "c17_test.h"

/* Prototype using [*] syntax (§6.7.5.2 p4) */
static int sum_vla_2d(int rows, int cols, int a[*][*]);

static int sum_vla_2d(int rows, int cols, int a[rows][cols]) {
    int s = 0;
    for (int i = 0; i < rows; i++) {
        for (int j = 0; j < cols; j++) {
            s += a[i][j];
        }
    }
    return s;
}

/* §6.7.7 p3: typedef of VLA evaluates size ONCE at typedef declaration! */
static int test_vla_typedef_snapshot(int n0) {
    int n = n0;
    typedef int Vec[n];
    n = 999; /* Must NOT affect Vec! */
    Vec v;
    for (int i = 0; i < n0; i++) {
        v[i] = i + 1;
    }
    if (sizeof(v) != (size_t)n0 * sizeof(int)) {
        return -1;
    }
    int s = 0;
    for (int i = 0; i < n0; i++) {
        s += v[i];
    }
    return s;
}

/* Loop stack deallocation: 4000 iterations x 4096 bytes = 16 MB > default 8 MB stack */
static int test_vla_loop_dealloc(int sz) {
    int acc = 0;
    for (int iter = 0; iter < 4000; iter++) {
        unsigned char buf[sz];
        buf[0] = (unsigned char)(iter & 0xff);
        buf[sz - 1] = (unsigned char)((iter + 1) & 0xff);
        acc += buf[0] + buf[sz - 1];
    }
    return acc;
}

/* Jumping out of a VLA block via goto inside a loop must also reclaim stack */
static int test_vla_goto_exit(int sz) {
    int acc = 0;
    for (int iter = 0; iter < 4000; iter++) {
        {
            unsigned char buf[sz];
            buf[0] = 1;
            buf[sz - 1] = 2;
            acc += buf[0] + buf[sz - 1];
            goto next_iter;
        }
next_iter:
        ;
    }
    return acc;
}

int main(void) {
    C99_BEGIN("c67_vla_comprehensive", "VLAs: 2D/3D, ptr-to-VLA, [*], typedef snapshot, loop/goto dealloc");

    /* 2D VLA + pointer-to-VLA arithmetic */
    int r = c99_opaque_int(4);
    int c = c99_opaque_int(5);
    int mat[r][c];
    C99_CHECK_EQ(sizeof(mat), (size_t)(r * c) * sizeof(int));
    C99_CHECK_EQ(sizeof(mat[0]), (size_t)c * sizeof(int));

    for (int i = 0; i < r; i++) {
        for (int j = 0; j < c; j++) {
            mat[i][j] = i * 10 + j;
        }
    }

    int (*row_ptr)[c] = mat;
    C99_CHECK_EQ(row_ptr[2][3], 23);
    C99_CHECK_EQ((*(row_ptr + 3))[4], 34);
    C99_CHECK_EQ(&row_ptr[4] - &row_ptr[1], 3);
    C99_CHECK_EQ(sum_vla_2d(r, c, mat), 340);

    /* 3D VLA */
    int d1 = c99_opaque_int(2);
    int d2 = c99_opaque_int(3);
    int d3 = c99_opaque_int(4);
    int cube[d1][d2][d3];
    C99_CHECK_EQ(sizeof(cube), 24 * sizeof(int));
    cube[1][2][3] = 777;
    C99_CHECK_EQ(cube[1][2][3], 777);

    /* typedef VLA snapshot */
    C99_CHECK_EQ(test_vla_typedef_snapshot(c99_opaque_int(6)), 21);

    /* Stack reclamation across loops and goto exits */
    int sz = c99_opaque_int(4096);
    C99_CHECK(test_vla_loop_dealloc(sz) > 0);
    C99_CHECK_EQ(test_vla_goto_exit(sz), 12000);

    C99_END();
}
