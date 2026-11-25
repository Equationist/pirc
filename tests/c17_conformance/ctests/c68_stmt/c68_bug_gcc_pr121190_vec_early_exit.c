/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.5 — Real-world compiler bug regressions:
 *   - GCC PR121190: early-exit search loops near buffer boundaries
 *   - gcc.c-torture/execute loop-ivopts: unsigned induction wraparound,
 *     non-unit negative strides, and (int)i - 2 underflow patterns
 */
#include <stdint.h>
#include "c17_test.h"

/* GCC PR121190 pattern: early exit must stop at exact index and never inspect
 * elements past len (guard element at buf[len] is set to target!). */
static int find_first(const int *buf, int len, int target) {
    for (int i = 0; i < len; i++) {
        if (buf[i] == target) {
            return i;
        }
    }
    return -1;
}

/* Early exit with live-out accumulator */
static int sum_until(const int *buf, int len, int stop_val, int *stopped_at) {
    int sum = 0;
    int i;
    for (i = 0; i < len; i++) {
        if (buf[i] == stop_val) {
            break;
        }
        sum += buf[i];
    }
    *stopped_at = i;
    return sum;
}

/* gcc.c-torture unsigned induction wraparound */
static unsigned wrap_count(unsigned start, unsigned end, unsigned step) {
    unsigned count = 0;
    for (unsigned u = start; u != end; u += step) {
        count++;
        if (count > 1000U) {
            break;
        }
    }
    return count;
}

/* gcc.c-torture unsigned-to-signed subtraction inside loop */
static int unsigned_iv_sub(int *out, unsigned n) {
    int sum = 0;
    for (unsigned i = 0; i < n; i++) {
        int v = (int)i - 2;
        out[i] = v;
        sum += v;
    }
    return sum;
}

int main(void) {
    C17_BEGIN("c68_bug_gcc_pr121190_vec_early_exit",
              "GCC PR121190 early-exit loops & unsigned IV wraparound");

    /* Test boundary lengths 0, 1, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33
     * with a poison sentinel matching target placed immediately at buf[len]. */
    {
        static const int lens[] = { 0, 1, 3, 4, 7, 8, 15, 16, 17, 31, 32, 33 };
        int buf[40];
        for (int t = 0; t < (int)(sizeof(lens) / sizeof(lens[0])); t++) {
            int n = c17_opaque_int(lens[t]);
            for (int i = 0; i < 40; i++) {
                buf[i] = i + 1;
            }
            /* Sentinel right past the end! */
            buf[n] = 999;
            C17_CHECK_EQ(find_first(buf, n, 999), -1);

            if (n > 0) {
                /* Match at last valid element n - 1 */
                buf[n - 1] = 999;
                C17_CHECK_EQ(find_first(buf, n, 999), n - 1);

                /* Match at first element 0 */
                buf[0] = 999;
                C17_CHECK_EQ(find_first(buf, n, 999), 0);
            }
        }
    }

    /* Live-out accumulator & induction variable after early break */
    {
        int data[10] = { 1, 2, 3, 4, 5, 99, 7, 8, 9, 10 };
        int pos = -1;
        int s = sum_until(data, c17_opaque_int(10), c17_opaque_int(99), &pos);
        C17_CHECK_EQ(s, 15);
        C17_CHECK_EQ(pos, 5);

        int pos2 = -1;
        int s2 = sum_until(data, c17_opaque_int(5), c17_opaque_int(99), &pos2);
        C17_CHECK_EQ(s2, 15);
        C17_CHECK_EQ(pos2, 5);
    }

    /* Unsigned wraparound across 0xFFFFFFFF -> 0 */
    {
        unsigned start = (unsigned)c17_opaque_ull(0xFFFFFFFCULL);
        unsigned end = (unsigned)c17_opaque_ull(0x00000004ULL);
        C17_CHECK_UEQ(wrap_count(start, end, 1U), 8U);
        C17_CHECK_UEQ(wrap_count(start, end, 2U), 4U);
        /* Negative stride modulo 2^32 (add 0xFFFFFFFE == -2) */
        C17_CHECK_UEQ(wrap_count(end, start, (unsigned)-2), 4U);
    }

    /* Unsigned IV converted to signed subtraction */
    {
        int out[5] = { 0 };
        int total = unsigned_iv_sub(out, (unsigned)c17_opaque_int(5));
        C17_CHECK_EQ(out[0], -2);
        C17_CHECK_EQ(out[1], -1);
        C17_CHECK_EQ(out[2], 0);
        C17_CHECK_EQ(out[3], 1);
        C17_CHECK_EQ(out[4], 2);
        C17_CHECK_EQ(total, 0);
    }

    C17_END();
}
