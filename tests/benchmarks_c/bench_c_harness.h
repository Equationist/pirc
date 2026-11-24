/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BENCH_C_HARNESS_H
#define BENCH_C_HARNESS_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static uint64_t bench_kernel(void);
static volatile uint64_t bench_sink;

int main(int argc, char **argv) {
    int iters = 15;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v > 0 && v <= 200) iters = v;
    }

    /* Call through a volatile function pointer so the compiler cannot
       hoist or CSE pure bench_kernel() calls across timing boundaries. */
    uint64_t (*volatile fn)(void) = bench_kernel;

    /* Warmup */
    uint64_t expected = fn();
    bench_sink = expected;

    uint64_t times[200];
    for (int i = 0; i < iters; ++i) {
        uint64_t t0 = bench_now_ns();
        uint64_t r = fn();
        uint64_t t1 = bench_now_ns();
        bench_sink = r;
        if (r != expected) {
            fprintf(stderr, "NON-DETERMINISTIC CHECKSUM: %" PRIx64 " vs %" PRIx64 "\n", r, expected);
            return 2;
        }
        times[i] = t1 - t0;
    }

    /* Sort for median */
    for (int i = 1; i < iters; ++i) {
        uint64_t key = times[i];
        int j = i - 1;
        while (j >= 0 && times[j] > key) {
            times[j + 1] = times[j];
            j--;
        }
        times[j + 1] = key;
    }

    double med_ms = (double)times[iters / 2] / 1e6;
    printf("CHECKSUM=0x%016" PRIx64 " MEDIAN_MS=%.3f\n", expected, med_ms);
    return 0;
}

#endif
