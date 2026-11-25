/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8 — Real-world compiler bug regressions:
 *   - GCC PR109342: loop-backedge value numbering propagating pre-loop predicate
 *   - GCC PR126760: switch inside nested loop with continue/break/fallthrough
 *   - Briggs-Cooper-Harvey SSA deconstruction swap & lost-copy problems
 */
#include "c17_test.h"

/* GCC PR109342 pattern: condition (flag == 0) is true on entry, becomes false
 * on backedge. Buggy SCCVN/GVN folds `if (flag == 0)` to always-true inside loop. */
static int vn_backedge_test(int n) {
    int flag = 0;
    int acc = 0;
    for (int i = 0; i < n; i++) {
        if (flag == 0) {
            acc += 10;
            flag = 1;
        } else {
            acc += 1;
            if ((i & 1) == 0) {
                flag = 0;
            }
        }
    }
    return acc;
}

/* Second backedge pattern: pointer/value updated conditionally on backedge */
static int vn_backedge_two_phase(int limit) {
    int x = 1;
    int y = 0;
    int total = 0;
    for (int i = 0; i < limit; i++) {
        if (x == 1) {
            total += y + 100;
        } else {
            total += x * 3 + y;
        }
        int next_x = (i == 0) ? 2 : (x + 1);
        y = x;
        x = next_x;
    }
    return total;
}

/* GCC PR126760 pattern: switch inside nested loop with continue & fallthrough */
static int nested_loop_switch(int rows, int cols) {
    int sum = 0;
    for (int r = 0; r < rows; r++) {
        int row_acc = 0;
        for (int c = 0; c < cols; c++) {
            switch ((r + c) % 5) {
            case 0:
                row_acc += 1;
                continue; /* continues inner loop, NOT outer */
            case 1:
                row_acc += 2;
                /* fallthrough */
            case 2:
                row_acc += 4;
                break; /* breaks switch, continues inner loop */
            case 3:
                row_acc += 8;
                if (c == cols - 1) {
                    continue;
                }
                row_acc += 16;
                break;
            default:
                row_acc += 32;
                break;
            }
            row_acc += 100;
        }
        sum += row_acc;
    }
    return sum;
}

/* Classic SSA deconstruction swap problem: simultaneous cycle a <- b <- c <- a */
static int ssa_phi_cycle(int a, int b, int c, int steps) {
    for (int i = 0; i < steps; i++) {
        int na = b;
        int nb = c;
        int nc = a;
        a = na;
        b = nb;
        c = nc;
    }
    return a * 100 + b * 10 + c;
}

/* Classic SSA deconstruction lost-copy problem: old value of x used after loop
 * while x is incremented in the loop body */
static int ssa_lost_copy(int n) {
    int x = 0;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        prev = x;
        x = x + 3;
        if (x >= 12) {
            break;
        }
    }
    return prev * 100 + x;
}

int main(void) {
    C17_BEGIN("c68_bug_gcc_pr109342_vn_backedge",
              "GCC PR109342 VN backedge, PR126760 nested switch, SSA phi swap/lost-copy");

    /* vn_backedge_test(6):
     * i=0: flag=0 -> acc=10, flag=1
     * i=1: flag=1 -> acc=11, i&1==1 -> flag=1
     * i=2: flag=1 -> acc=12, i&1==0 -> flag=0
     * i=3: flag=0 -> acc=22, flag=1
     * i=4: flag=1 -> acc=23, i&1==0 -> flag=0
     * i=5: flag=0 -> acc=33, flag=1
     */
    C17_CHECK_EQ(vn_backedge_test(c17_opaque_int(6)), 33);
    C17_CHECK_EQ(vn_backedge_test(c17_opaque_int(1)), 10);
    C17_CHECK_EQ(vn_backedge_test(c17_opaque_int(0)), 0);

    /* vn_backedge_two_phase(5):
     * i=0: x=1, y=0 -> total=100; y=1, x=2
     * i=1: x=2, y=1 -> total=100+7=107; y=2, x=3
     * i=2: x=3, y=2 -> total=107+11=118; y=3, x=4
     * i=3: x=4, y=3 -> total=118+15=133; y=4, x=5
     * i=4: x=5, y=4 -> total=133+19=152; y=5, x=6
     */
    C17_CHECK_EQ(vn_backedge_two_phase(c17_opaque_int(5)), 152);

    /* nested_loop_switch(3, 4):
     * r=0: c=0(0):+1; c=1(1):+2+4+100=106; c=2(2):+4+100=104; c=3(3,last):+8 -> 219
     * r=1: c=0(1):106; c=1(2):104; c=2(3):+8+16+100=124; c=3(4):+32+100=132 -> 466
     * r=2: c=0(2):104; c=1(3):124; c=2(4):132; c=3(0):+1 -> 361
     * total = 219 + 466 + 361 = 1046
     */
    C17_CHECK_EQ(nested_loop_switch(c17_opaque_int(3), c17_opaque_int(4)), 1046);

    /* SSA phi 3-cycle */
    C17_CHECK_EQ(ssa_phi_cycle(c17_opaque_int(1), c17_opaque_int(2),
                               c17_opaque_int(3), c17_opaque_int(1)), 231);
    C17_CHECK_EQ(ssa_phi_cycle(c17_opaque_int(1), c17_opaque_int(2),
                               c17_opaque_int(3), c17_opaque_int(2)), 312);
    C17_CHECK_EQ(ssa_phi_cycle(c17_opaque_int(1), c17_opaque_int(2),
                               c17_opaque_int(3), c17_opaque_int(3)), 123);
    C17_CHECK_EQ(ssa_phi_cycle(c17_opaque_int(1), c17_opaque_int(2),
                               c17_opaque_int(3), c17_opaque_int(7)), 231);

    /* SSA lost-copy: breaks at i=3 when x goes 9 -> 12, prev == 9 */
    C17_CHECK_EQ(ssa_lost_copy(c17_opaque_int(10)), 912);
    C17_CHECK_EQ(ssa_lost_copy(c17_opaque_int(2)), 306);

    C17_END();
}
