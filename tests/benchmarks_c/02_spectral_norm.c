/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Classic Spectral-Norm eigenvalue benchmark (CLBG, pure C99). */
#include <math.h>
#include "bench_c_harness.h"

static double eval_A(int i, int j) {
    return 1.0 / (double)(((i + j) * (i + j + 1)) / 2 + i + 1);
}

static void eval_A_times_u(int N, const double * restrict u, double * restrict Au) {
    for (int i = 0; i < N; ++i) {
        double sum = 0.0;
        for (int j = 0; j < N; ++j) {
            sum += eval_A(i, j) * u[j];
        }
        Au[i] = sum;
    }
}

static void eval_At_times_u(int N, const double * restrict u, double * restrict Au) {
    for (int i = 0; i < N; ++i) {
        double sum = 0.0;
        for (int j = 0; j < N; ++j) {
            sum += eval_A(j, i) * u[j];
        }
        Au[i] = sum;
    }
}

static void eval_AtA_times_u(int N, const double * restrict u, double * restrict AtAu,
                             double * restrict tmp) {
    eval_A_times_u(N, u, tmp);
    eval_At_times_u(N, tmp, AtAu);
}

static uint64_t bench_kernel(void) {
    const int N = 500;
    double *u = (double *)malloc((size_t)N * sizeof(double));
    double *v = (double *)malloc((size_t)N * sizeof(double));
    double *tmp = (double *)malloc((size_t)N * sizeof(double));
    for (int i = 0; i < N; ++i) u[i] = 1.0;

    for (int i = 0; i < 10; ++i) {
        eval_AtA_times_u(N, u, v, tmp);
        eval_AtA_times_u(N, v, u, tmp);
    }

    double vBv = 0.0, vv = 0.0;
    for (int i = 0; i < N; ++i) {
        vBv += u[i] * v[i];
        vv += v[i] * v[i];
    }
    double norm = sqrt(vBv / vv);
    free(u);
    free(v);
    free(tmp);
    return (uint64_t)(int64_t)(norm * 1e9);
}
