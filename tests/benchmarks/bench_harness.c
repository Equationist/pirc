/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern uint64_t bench_run(void);

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

int main(int argc, char **argv) {
    int iters = 5;
    if (argc >= 2) {
        iters = atoi(argv[1]);
        if (iters < 1) iters = 1;
    }

    /* Warmup + reference checksum */
    uint64_t checksum = bench_run();

    double best_ms = 1e30;
    for (int i = 0; i < iters; ++i) {
        double t0 = now_ms();
        uint64_t c = bench_run();
        double dt = now_ms() - t0;
        if (c != checksum) {
            fprintf(stderr, "FAIL: non-deterministic checksum 0x%" PRIx64 " != 0x%" PRIx64 "\n",
                    c, checksum);
            return 1;
        }
        if (dt < best_ms) best_ms = dt;
    }

    printf("CHECKSUM=0x%016" PRIx64 " BEST_MS=%.3f\n", checksum, best_ms);
    return 0;
}
