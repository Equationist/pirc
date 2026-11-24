/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Sieve of Eratosthenes up to 2,000,000 (byte-array stride & branch workload, pure C99). */
#include <string.h>
#include "bench_c_harness.h"

static uint64_t bench_kernel(void) {
    const int N = 2000000;
    uint8_t *is_comp = (uint8_t *)malloc((size_t)(N + 1));
    memset(is_comp, 0, (size_t)(N + 1));
    is_comp[0] = 1;
    is_comp[1] = 1;

    for (int p = 2; p * p <= N; ++p) {
        if (!is_comp[p]) {
            for (int m = p * p; m <= N; m += p) {
                is_comp[m] = 1;
            }
        }
    }

    uint64_t count = 0;
    uint64_t sum = 0;
    for (int i = 2; i <= N; ++i) {
        if (!is_comp[i]) {
            count++;
            sum += (uint64_t)i;
        }
    }
    free(is_comp);
    return (count << 32) ^ sum;
}
