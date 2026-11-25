/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.3.4 / §6.7.6.2 — Real-world VLA compiler bugs:
 *   - typedef int vla_t[n++] evaluates n++ ONCE at typedef point, NOT on sizeof(vla_t)
 *   - sizeof(int[side_effect()]) MUST evaluate side_effect() (C17 §6.5.3.4 p2)
 *   - Forward prototype with [*] VLA parameter & 2D VLA indexing
 *   - Loop-scoped VLA with continue/break/goto stack reclamation (no stack leak)
 */
#include <stddef.h>
#include "c17_test.h"

static int g_calls = 0;

static int next_len(void) {
    g_calls++;
    return 4 + g_calls;
}

/* Forward prototype with [*] VLA syntax (C17 §6.7.6.2 p4) */
static int sum_matrix(int rows, int cols, int m[*][*]);

static int sum_matrix(int rows, int cols, int m[rows][cols]) {
    int total = 0;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            total += m[r][c];
        }
    }
    return total;
}

static int loop_vla_reclaim(int iters, int len) {
    int acc = 0;
    for (int i = 0; i < iters; i++) {
        int buf[len]; /* ~1KB per iter; 2000 iters = 2MB -> overflows if not reclaimed */
        buf[0] = i;
        buf[len - 1] = i + 1;
        if ((i & 3) == 0) {
            acc += buf[0];
            continue;
        }
        if (i == iters - 1) {
            acc += buf[len - 1];
            goto done;
        }
        acc += buf[len - 1] - buf[0];
    }
done:
    return acc;
}

int main(void) {
    C17_BEGIN("c67_bug_vla_sizeof_sideeffect",
              "VLA typedef snapshot, sizeof side-effect, [*] 2D VLA, loop reclamation");

    /* typedef VLA evaluates dimension expression ONCE */
    {
        int n = c17_opaque_int(5);
        typedef int vla_t[n++];
        C17_CHECK_EQ(n, 6);
        size_t sz1 = sizeof(vla_t);
        size_t sz2 = sizeof(vla_t);
        C17_CHECK_EQ(n, 6); /* sizeof(vla_t) must NOT re-evaluate n++! */
        C17_CHECK_EQ(sz1, 5 * sizeof(int));
        C17_CHECK_EQ(sz2, 5 * sizeof(int));
    }

    /* sizeof on VLA type name MUST evaluate its runtime dimension expression */
    {
        g_calls = 0;
        size_t sz1 = sizeof(int[next_len()]); /* next_len() -> 5, g_calls -> 1 */
        size_t sz2 = sizeof(int[next_len()]); /* next_len() -> 6, g_calls -> 2 */
        C17_CHECK_EQ(g_calls, 2);
        C17_CHECK_EQ(sz1, 5 * sizeof(int));
        C17_CHECK_EQ(sz2, 6 * sizeof(int));
    }

    /* 2D VLA passed to function declared with [*] */
    {
        int r = c17_opaque_int(3);
        int c = c17_opaque_int(4);
        int mat[r][c];
        int k = 1;
        for (int i = 0; i < r; i++) {
            for (int j = 0; j < c; j++) {
                mat[i][j] = k++;
            }
        }
        /* 1 + 2 + ... + 12 = 78 */
        C17_CHECK_EQ(sum_matrix(r, c, mat), 78);
    }

    /* Loop-scoped VLA stack reclamation with continue & goto */
    {
        int iters = c17_opaque_int(2000);
        int len = c17_opaque_int(256);
        int got = loop_vla_reclaim(iters, len);
        /* Compute expected:
         * i=0..1999:
         *   i%4==0 (500 values: 0,4,8,...,1996): sum = 500 * 1996 / 2 = 499000
         *   i==1999 (1999%4==3): adds 2000
         *   remaining 1499 values: each adds 1 -> +1499
         * total = 499000 + 2000 + 1499 = 502499
         */
        C17_CHECK_EQ(got, 502499);
    }

    C17_END();
}
