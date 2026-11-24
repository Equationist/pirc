/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Iterative Cooley-Tukey Radix-2 FFT (N = 65536, double precision, pure C99). */
#include <math.h>
#include "bench_c_harness.h"

#define FFT_PI 3.14159265358979323846

static void fft_radix2(double * restrict re, double * restrict im, int n) {
    /* Bit-reversal permutation */
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double tr = re[i]; re[i] = re[j]; re[j] = tr;
            double ti = im[i]; im[i] = im[j]; im[j] = ti;
        }
    }

    /* Butterfly stages */
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * FFT_PI / (double)len;
        double wlen_r = cos(ang);
        double wlen_i = sin(ang);
        int half = len >> 1;
        for (int i = 0; i < n; i += len) {
            double wr = 1.0, wi = 0.0;
            for (int j = 0; j < half; ++j) {
                int idx1 = i + j;
                int idx2 = idx1 + half;
                double ur = re[idx1], ui = im[idx1];
                double vr = re[idx2] * wr - im[idx2] * wi;
                double vi = re[idx2] * wi + im[idx2] * wr;
                re[idx1] = ur + vr;
                im[idx1] = ui + vi;
                re[idx2] = ur - vr;
                im[idx2] = ui - vi;
                double nwr = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = nwr;
            }
        }
    }
}

static uint64_t bench_kernel(void) {
    const int N = 65536;
    double *re = (double *)malloc((size_t)N * sizeof(double));
    double *im = (double *)malloc((size_t)N * sizeof(double));
    for (int i = 0; i < N; ++i) {
        re[i] = (double)((i & 63) - 32) * 0.0625;
        im[i] = (double)(((i * 5) & 63) - 32) * 0.0625;
    }

    fft_radix2(re, im, N);

    double energy = 0.0;
    for (int i = 0; i < N; ++i) {
        energy += re[i] * re[i] + im[i] * im[i];
    }
    free(re);
    free(im);
    /* Quantize to integer energy (Parseval's theorem invariant across FMA rounding) */
    return (uint64_t)(int64_t)(energy + 0.5);
}
