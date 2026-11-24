/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdlib.h>

static void matmul_kernel(float * restrict C, const float * restrict A,
                          const float * restrict B, int64_t n) {
    for (int64_t i = 0; i < n; ++i) {
        for (int64_t j = 0; j < n; ++j) {
            float acc = 0.0f;
            for (int64_t k = 0; k < n; ++k) {
                acc += A[i * n + k] * B[k * n + j];
            }
            C[i * n + j] = acc;
        }
    }
}

uint64_t bench_run(void) {
    const int64_t N = 128;
    const int64_t M = N * N;
    float *A = (float *)malloc((size_t)M * sizeof(float));
    float *B = (float *)malloc((size_t)M * sizeof(float));
    float *C = (float *)malloc((size_t)M * sizeof(float));

    for (int64_t i = 0; i < M; ++i) {
        A[i] = (float)((i * 7 + 3) & 15) * 0.25f;
        B[i] = (float)((i * 11 + 5) & 15) * 0.25f;
    }

    matmul_kernel(C, A, B, N);

    uint64_t chk = 0;
    for (int64_t i = 0; i < M; ++i) {
        chk += (uint64_t)(int64_t)C[i];
    }

    free(A);
    free(B);
    free(C);
    return chk;
}
