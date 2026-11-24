/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>
#include <stdlib.h>

static void simd_clamp_kernel(float * restrict dst, const float * restrict src,
                              int64_t n, float gain, float lo, float hi) {
    for (int64_t i = 0; i < n; ++i) {
        float v = src[i] * gain;
        if (v > hi) v = hi;
        if (v < lo) v = lo;
        dst[i] = v;
    }
}

uint64_t bench_run(void) {
    const int64_t N = 262144;
    float *src = (float *)malloc((size_t)N * sizeof(float));
    float *dst = (float *)malloc((size_t)N * sizeof(float));
    for (int64_t i = 0; i < N; ++i) {
        src[i] = (float)((i & 31) - 16) * 0.25f;
    }

    for (int rep = 0; rep < 64; ++rep) {
        simd_clamp_kernel(dst, src, N, 1.5f, -2.0f, 2.0f);
    }

    uint64_t chk = 0;
    for (int64_t i = 0; i < N; i += 64) {
        int32_t q = (int32_t)(dst[i] * 16.0f);
        chk = chk * 131u + (uint64_t)(uint32_t)q;
    }
    free(src);
    free(dst);
    return chk;
}
