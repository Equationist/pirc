/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.6.1 — Real-world SSA / CFG optimizer bugs:
 *   - Irreducible (multi-entry) loops constructed via `goto` into loop bodies:
 *     stresses dominators, loop detection, mem2reg, GVN, LICM, DSE, and regalloc
 *   - Backward `goto` re-entering block scope with local re-initialization
 *   - 4-deep nested loop (YARPGen pattern) with multi-level state & breaks
 */
#include "c17_test.h"

/* Classic irreducible CFG: loop has TWO entry points (normal entry + goto mid_loop).
 * Every SSA pass that assumes all loops are reducible (single-header natural loops)
 * risks miscomputing dominance, phi placement, or LICM hoisting! */
static int irreducible_loop(int start_mid, int limit) {
    int acc = 0;
    int i = 0;
    if (start_mid) {
        acc = 100;
        i = 1;
        goto mid_loop;
    }
    while (i < limit) {
        acc += i * 2 + 1;
    mid_loop:
        acc += 10;
        i++;
        if (i < limit && (i & 1) == 0) {
            goto top_again;
        }
        continue;
    top_again:
        acc += 1000;
    }
    return acc;
}

/* Second irreducible CFG: mutually branching goto cycles (A <-> B) */
static int mutual_goto_cycle(int mode, int n) {
    int x = 1;
    int y = 2;
    if (mode == 1) goto lab_b;
lab_a:
    if (n <= 0) return x * 100 + y;
    x += y;
    n--;
    if ((n & 1) == 0) goto lab_a;
lab_b:
    if (n <= 0) return x * 100 + y;
    y += x;
    n--;
    goto lab_a;
}

/* Backward goto across a block-scoped variable declaration */
static int backward_goto_scope(int count) {
    int total = 0;
    int k = 0;
again:
    {
        int local_val = k * 5 + 1;
        total += local_val;
    }
    k++;
    if (k < count) {
        goto again;
    }
    return total;
}

/* 4-deep nested loop (YARPGen-style loop nest) */
static int yarpgen_4deep(int a, int b, int c, int d) {
    int acc = 0;
    for (int i = 0; i < a; i++) {
        for (int j = 0; j < b; j++) {
            if (i == j) continue;
            for (int k = 0; k < c; k++) {
                for (int l = 0; l < d; l++) {
                    acc = (acc * 3 + i * 7 + j * 5 + k * 2 + l) & 0xFFFF;
                    if (l == k && l > 1) break;
                }
            }
        }
    }
    return acc;
}

int main(void) {
    C17_BEGIN("c68_bug_irreducible_cfg_loops",
              "Irreducible multi-entry goto loops, mutual goto cycles, 4-deep nest");

    /* Trace irreducible_loop(0, 4):
     * i=0: acc+=1(1), mid:acc+=10(11), i=1 (odd)->continue
     * i=1: acc+=3(14), mid:acc+=10(24), i=2 (even)->top_again: acc+=1000(1024)
     * i=2: acc+=5(1029), mid:acc+=10(1039), i=3 (odd)->continue
     * i=3: acc+=7(1046), mid:acc+=10(1056), i=4 -> loop ends
     */
    C17_CHECK_EQ(irreducible_loop(c17_opaque_int(0), c17_opaque_int(4)), 1056);

    /* Trace irreducible_loop(1, 4):
     * start_mid: acc=100, i=1 -> mid: acc+=10(110), i=2 (even)->top_again: acc+=1000(1110)
     * i=2: acc+=5(1115), mid:acc+=10(1125), i=3 (odd)->continue
     * i=3: acc+=7(1132), mid:acc+=10(1142), i=4 -> loop ends
     */
    C17_CHECK_EQ(irreducible_loop(c17_opaque_int(1), c17_opaque_int(4)), 1142);

    /* Trace mutual_goto_cycle(0, 4):
     * start: x=1,y=2,n=4
     * lab_a: x=3, n=3 (odd)->lab_b: y=5, n=2 -> lab_a: x=8, n=1 (odd)->lab_b: y=13, n=0 -> ret 813
     */
    C17_CHECK_EQ(mutual_goto_cycle(c17_opaque_int(0), c17_opaque_int(4)), 813);
    /* Trace mutual_goto_cycle(1, 4):
     * lab_b: y=3, n=3 -> lab_a: x=4, n=2 (even)->lab_a: x=7, n=1 (odd)->lab_b: y=10, n=0 -> ret 710
     */
    C17_CHECK_EQ(mutual_goto_cycle(c17_opaque_int(1), c17_opaque_int(4)), 710);

    /* backward_goto_scope(5): 1 + 6 + 11 + 16 + 21 = 55 */
    C17_CHECK_EQ(backward_goto_scope(c17_opaque_int(5)), 55);

    /* yarpgen_4deep(3, 3, 3, 4) checked against opaque-stepped reference */
    {
        int got = yarpgen_4deep(c17_opaque_int(3), c17_opaque_int(3),
                                c17_opaque_int(3), c17_opaque_int(4));
        int ref = 0;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                if (i == j) continue;
                for (int k = 0; k < 3; k++) {
                    for (int l = 0; l < 4; l++) {
                        volatile int step = (ref * 3 + i * 7 + j * 5 + k * 2 + l) & 0xFFFF;
                        ref = step;
                        if (l == k && l > 1) break;
                    }
                }
            }
        }
        C17_CHECK_EQ(got, ref);
    }

    C17_END();
}
