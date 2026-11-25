/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.5, §6.7.2.1, §6.7.6.2 — File-scope & loop-scope
 * compound literal lifetimes, zero-width & unnamed padding bitfields, functions
 * returning pointer-to-array, and 2D VLA pointer parameters.
 */
#include <stddef.h>
#include <stdint.h>
#include "c17_test.h"

struct pair_i {
    int a;
    int b;
};

/* File-scope compound literal has static storage duration */
static struct pair_i *g_cl_ptr = &(struct pair_i){ 10, 20 };

static int bump_file_cl(int delta) {
    g_cl_ptr->a += delta;
    g_cl_ptr->b -= delta;
    return g_cl_ptr->a * 100 + g_cl_ptr->b;
}

/* Zero-width bitfield forces alignment to next unsigned int unit;
 * unnamed bitfield adds padding bits without a member name. */
struct bf_zero_pad {
    unsigned a : 5;
    unsigned : 3;     /* unnamed padding */
    unsigned b : 4;
    unsigned : 0;     /* force next storage unit */
    unsigned c : 6;
};

/* Function returning pointer to array of 4 ints */
static int (*select_row(int (*mat)[4], int row))[4] {
    return &mat[row];
}

/* Function taking 2D VLA pointer parameter */
static int sum_vla2d(int rows, int cols, int (*mat)[cols]) {
    int s = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            s += mat[r][c];
        }
    }
    return s;
}

static int (*(*volatile g_sel_row)(int (*)[4], int))[4] = select_row;
static int (*volatile g_sum_vla2d)(int, int, int (*)[]) = sum_vla2d;

int main(void) {
    C17_BEGIN("c67_bug_compound_lit_lifetime_bitfield_zero",
              "Compound literal lifetimes, :0 bitfields, ptr-to-array ret & 2D VLA");

    /* 1. File-scope compound literal persists across calls */
    C17_CHECK_EQ(bump_file_cl(c17_opaque_int(3)), 1317);
    C17_CHECK_EQ(bump_file_cl(c17_opaque_int(2)), 1515);

    /* 2. Block-scope compound literal re-initialized every loop iteration */
    {
        int acc = 0;
        for (int i = 1; i <= 5; i++) {
            struct pair_i *p = &(struct pair_i){ i, i * 10 };
            acc += p->a + p->b;
            /* Mutate through pointer; next iteration MUST re-initialize! */
            p->a = 9999;
            p->b = 9999;
        }
        /* sum_{i=1..5} (11*i) = 11 * 15 = 165 */
        C17_CHECK_EQ(acc, 165);
    }

    /* 3. Multi-dimensional array compound literal */
    {
        int (*m)[3] = (int [2][3]){ { 1, 2, 3 }, { 4, 5, 6 } };
        C17_CHECK_EQ(m[0][0] + m[0][2] + m[1][1] + m[1][2], 15);
    }

    /* 4. Zero-width and unnamed padding bitfields */
    {
        C17_CHECK(sizeof(struct bf_zero_pad) >= 2u * sizeof(unsigned));
        struct bf_zero_pad z = { 0 };
        z.a = 31u;
        z.b = 15u;
        z.c = 63u;
        C17_CHECK_EQ(z.a, 31u);
        C17_CHECK_EQ(z.b, 15u);
        C17_CHECK_EQ(z.c, 63u);
        z.b = 5u;
        C17_CHECK_EQ(z.a, 31u);
        C17_CHECK_EQ(z.b, 5u);
        C17_CHECK_EQ(z.c, 63u);
    }

    /* 5. Function returning pointer-to-array & 2D VLA parameter */
    {
        int mat[3][4] = {
            { 1, 2, 3, 4 },
            { 10, 20, 30, 40 },
            { 100, 200, 300, 400 }
        };
        int (*rp)[4] = g_sel_row(mat, c17_opaque_int(1));
        C17_CHECK_EQ((*rp)[0] + (*rp)[3], 50);

        int r = c17_opaque_int(3);
        int c = c17_opaque_int(5);
        int vla[r][c];
        int expect = 0;
        for (int i = 0; i < r; i++) {
            for (int j = 0; j < c; j++) {
                vla[i][j] = i * 10 + j + 1;
                expect += vla[i][j];
            }
        }
        C17_CHECK_EQ(sum_vla2d(r, c, vla), expect);
        (void)g_sum_vla2d;
    }

    C17_END();
}
