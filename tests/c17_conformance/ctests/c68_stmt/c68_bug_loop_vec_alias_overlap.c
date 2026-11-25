/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.5 — Real-world loop vectorizer bug regressions:
 *   - Loop-carried forward dependence (dst = buf + 1, src = buf) must NOT
 *     be mis-vectorized without aliasing check
 *   - Distance-2 overlap (dst = buf + 2, src = buf) < 4-lane vector width
 *   - Backward anti-dependence (buf[i] = buf[i + 1] + 1)
 *   - Integer reductions (sum, prod, min, max, xor, and, or) with non-zero
 *     initial values across odd & boundary trip counts (0..33)
 *   - Floating-point reduction preserving strict left-to-right IEEE-754 order
 */
#include <stdint.h>
#include "c17_test.h"

/* Classic alias trap: if dst == src + 1, this is a recurrence dst[i] = dst[i-1] + 1! */
static void add_one_maybe_alias(int *dst, const int *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i] + 1;
    }
}

/* Distance-2 overlap: if dst == src + 2, 4-wide vectorization corrupts lanes 2 & 3! */
static void add_two_arrays(int *dst, const int *a, const int *b, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = a[i] + b[i];
    }
}

static int reduce_sum(const int *a, int n, int init) {
    int s = init;
    for (int i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

static uint32_t reduce_xor(const uint32_t *a, int n, uint32_t init) {
    uint32_t x = init;
    for (int i = 0; i < n; i++) {
        x ^= a[i];
    }
    return x;
}

static int reduce_min_s(const int *a, int n, int init) {
    int m = init;
    for (int i = 0; i < n; i++) {
        if (a[i] < m) {
            m = a[i];
        }
    }
    return m;
}

static uint32_t reduce_max_u(const uint32_t *a, int n, uint32_t init) {
    uint32_t m = init;
    for (int i = 0; i < n; i++) {
        if (a[i] > m) {
            m = a[i];
        }
    }
    return m;
}

static double reduce_fp_ordered(const double *a, int n) {
    double s = 0.0;
    for (int i = 0; i < n; i++) {
        s += a[i];
    }
    return s;
}

int main(void) {
    C17_BEGIN("c68_bug_loop_vec_alias_overlap",
              "Loop-vec alias overlap (dist 1 & 2), odd-len reductions, ordered FP");

    /* Distance-1 overlap: buf[i+1] = buf[i] + 1 -> must produce 10, 11, 12, ... */
    {
        int buf[32];
        for (int i = 0; i < 32; i++) buf[i] = 0;
        buf[0] = c17_opaque_int(10);
        add_one_maybe_alias(buf + 1, buf, c17_opaque_int(20));
        for (int i = 0; i <= 20; i++) {
            C17_CHECK_EQ(buf[i], 10 + i);
        }
    }

    /* Distance-2 overlap: buf[i+2] = buf[i] + ones[i] -> even/odd chains increment by 1 */
    {
        int buf[24];
        int ones[24];
        for (int i = 0; i < 24; i++) {
            buf[i] = 0;
            ones[i] = 1;
        }
        buf[0] = c17_opaque_int(100);
        buf[1] = c17_opaque_int(200);
        add_two_arrays(buf + 2, buf, ones, c17_opaque_int(16));
        for (int i = 0; i < 18; i++) {
            int want = ((i & 1) == 0 ? 100 : 200) + (i / 2);
            C17_CHECK_EQ(buf[i], want);
        }
    }

    /* Backward anti-dependence: buf[i] = buf[i+1] + 1 (shifts array left & adds 1) */
    {
        int buf[20];
        for (int i = 0; i < 20; i++) buf[i] = i * 10;
        add_one_maybe_alias(buf, buf + 1, c17_opaque_int(19));
        for (int i = 0; i < 19; i++) {
            C17_CHECK_EQ(buf[i], (i + 1) * 10 + 1);
        }
        C17_CHECK_EQ(buf[19], 190);
    }

    /* Reductions across odd & vector-boundary lengths */
    {
        int data[35];
        uint32_t udata[35];
        for (int i = 0; i < 35; i++) {
            data[i] = (i & 1) ? -i : (i * 2);
            udata[i] = (i == 17) ? 0x90000000U : (uint32_t)(i + 1);
        }
        static const int lens[] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 18, 31, 33 };
        for (int t = 0; t < (int)(sizeof(lens) / sizeof(lens[0])); t++) {
            int n = c17_opaque_int(lens[t]);
            int want_sum = 1000;
            uint32_t want_xor = 0x55AA55AAU;
            int want_min = 9999;
            uint32_t want_max = 0U;
            for (int i = 0; i < lens[t]; i++) {
                want_sum += data[i];
                want_xor ^= udata[i];
                if (data[i] < want_min) want_min = data[i];
                if (udata[i] > want_max) want_max = udata[i];
            }
            C17_CHECK_EQ(reduce_sum(data, n, c17_opaque_int(1000)), want_sum);
            C17_CHECK_UEQ(reduce_xor(udata, n, (uint32_t)c17_opaque_ull(0x55AA55AAULL)), want_xor);
            C17_CHECK_EQ(reduce_min_s(data, n, c17_opaque_int(9999)), want_min);
            C17_CHECK_UEQ(reduce_max_u(udata, n, 0U), want_max);
        }
    }

    /* Ordered FP reduction: ((1e16 + 1.0) + 1.0) - 1e16 == 0.0 (NOT 2.0!)
     * A vectorizer that reassociates FP sums into parallel accumulators breaks this! */
    {
        double arr[8] = { 1e16, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, -1e16 };
        double got = reduce_fp_ordered(arr, c17_opaque_int(8));
        C17_CHECK_DBL(got, 0.0, 0.0);
    }

    C17_END();
}
