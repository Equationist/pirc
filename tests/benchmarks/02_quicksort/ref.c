/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdlib.h>

static int64_t partition(int32_t * restrict arr, int64_t lo, int64_t hi) {
    int32_t pivot = arr[hi];
    int64_t i = lo;
    for (int64_t j = lo; j < hi; ++j) {
        if (arr[j] <= pivot) {
            int32_t tmp = arr[i];
            arr[i] = arr[j];
            arr[j] = tmp;
            ++i;
        }
    }
    int32_t tmp = arr[i];
    arr[i] = pivot;
    arr[hi] = tmp;
    return i;
}

static void qsort_rec(int32_t * restrict arr, int64_t lo, int64_t hi) {
    if (lo < hi) {
        int64_t p = partition(arr, lo, hi);
        qsort_rec(arr, lo, p - 1);
        qsort_rec(arr, p + 1, hi);
    }
}

uint64_t bench_run(void) {
    const int64_t N = 65536;
    int32_t *arr = (int32_t *)malloc((size_t)N * sizeof(int32_t));
    uint32_t st = 0x12345678u;
    for (int64_t i = 0; i < N; ++i) {
        st = st * 1664525u + 1013904223u;
        arr[i] = (int32_t)st;
    }

    qsort_rec(arr, 0, N - 1);

    uint64_t chk = 0;
    for (int64_t i = 0; i < N; ++i) {
        chk = chk * 31u + (uint64_t)(uint32_t)arr[i];
    }
    free(arr);
    return chk;
}
