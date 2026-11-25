/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 — Adversarial auto-vectorizer (loop_vec.c)
 * stress tests:
 *  - Negative-start i32 induction variable with overlapping/disjoint slices
 *  - Signed arithmetic right shift vs unsigned logical right shift (const & var)
 *  - Vectorized branchless select (int32, uint32, int64, uint64, float, double,
 *    and float compare selecting int32)
 *  - 64-bit vector multiply & unsigned compare
 *  - Multiple reductions, downward loops, stride-2 loops
 */
#include <stdint.h>
#include "c17_test.h"

/* 1. Negative starting i32 IV: exercises loop_vec negative iv_init sign-extension! */
static void neg_iv_copy(int32_t *dst, const int32_t *src, int n) {
    for (int i = -16; i < n; i++) {
        dst[i + 16] = src[i + 16] + 1;
    }
}

/* 2. Signed vs unsigned right shifts (constant and runtime shift count) */
static void shift_signed_const(int32_t *dst, const int32_t *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i] >> 3;
    }
}

static void shift_unsigned_var(uint32_t *dst, const uint32_t *src, int n, unsigned k) {
    for (int i = 0; i < n; i++) {
        dst[i] = (src[i] >> k) ^ (src[i] << (32u - k));
    }
}

static void shift_per_lane(uint32_t *dst, const uint32_t *src, const uint32_t *sh, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[i] >> (sh[i] & 15u);
    }
}

/* 3. Vectorized branchless select across signed/unsigned 32/64 & mixed float->int */
static void select_u64_and_mul(uint64_t *dst, const uint64_t *a, const uint64_t *b, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t prod = a[i] * b[i];
        dst[i] = (a[i] < b[i]) ? prod : (a[i] - b[i]);
    }
}

static void select_f32_to_i32(int32_t *dst, const float *fa, const float *fb,
                              const int32_t *ia, const int32_t *ib, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = (fa[i] < fb[i]) ? ia[i] : ib[i];
    }
}

/* 4. Multi-reduction + downward loop */
static int64_t multi_reduce(const int32_t *a, int n, int32_t *out_min, int32_t *out_max, uint32_t *out_xor) {
    int64_t sum = 0;
    int32_t mn = INT32_MAX, mx = INT32_MIN;
    uint32_t xr = 0u;
    for (int i = n - 1; i >= 0; i--) {
        int32_t v = a[i];
        sum += v;
        if (v < mn) mn = v;
        if (v > mx) mx = v;
        xr ^= (uint32_t)v;
    }
    *out_min = mn;
    *out_max = mx;
    *out_xor = xr;
    return sum;
}

static void (*volatile g_neg_iv)(int32_t *, const int32_t *, int) = neg_iv_copy;
static void (*volatile g_sar_c)(int32_t *, const int32_t *, int) = shift_signed_const;
static void (*volatile g_shr_v)(uint32_t *, const uint32_t *, int, unsigned) = shift_unsigned_var;
static void (*volatile g_shr_l)(uint32_t *, const uint32_t *, const uint32_t *, int) = shift_per_lane;
static void (*volatile g_sel_u64)(uint64_t *, const uint64_t *, const uint64_t *, int) = select_u64_and_mul;
static void (*volatile g_sel_fi)(int32_t *, const float *, const float *, const int32_t *, const int32_t *, int) = select_f32_to_i32;
static int64_t (*volatile g_mred)(const int32_t *, int, int32_t *, int32_t *, uint32_t *) = multi_reduce;

int main(void) {
    C17_BEGIN("c65_bug_loop_vec_shifts_select_negiv",
              "loop_vec negative IV, signed/unsigned shifts, 64-bit mul/select, multi-red");

    /* 1. Negative-start IV with BOTH disjoint and overlapping buffers! */
    {
        int32_t buf[60];
        for (int i = 0; i < 60; i++) buf[i] = i * 2;
        /* Overlapping: dst = buf + 1, src = buf -> propagates buf[0] + (i+1)! */
        g_neg_iv(buf + 1, buf, c17_opaque_int(30)); /* 46 iterations: i = -16..29 */
        for (int i = 0; i <= 46; i++) {
            C17_CHECK_EQ(buf[i], i);
        }

        /* Disjoint negative-start IV */
        int32_t src[50], dst[50];
        for (int i = 0; i < 50; i++) { src[i] = -100 + i * 3; dst[i] = 0; }
        g_neg_iv(dst, src, c17_opaque_int(25)); /* 41 iterations */
        for (int i = 0; i < 41; i++) C17_CHECK_EQ(dst[i], src[i] + 1);
        for (int i = 41; i < 50; i++) C17_CHECK_EQ(dst[i], 0);
    }

    /* 2. Signed arithmetic vs unsigned logical shifts */
    {
        int32_t s_in[35], s_out[35];
        for (int i = 0; i < 35; i++) s_in[i] = (i & 1) ? -(i * 17 + 8) : (i * 17 + 8);
        g_sar_c(s_out, s_in, c17_opaque_int(35));
        for (int i = 0; i < 35; i++) {
            C17_CHECK_EQ(s_out[i], s_in[i] >> 3);
        }

        uint32_t u_in[35], u_out[35], u_sh[35];
        for (int i = 0; i < 35; i++) {
            u_in[i] = UINT32_C(0x80000001) + (uint32_t)i * UINT32_C(0x01020304);
            u_sh[i] = (uint32_t)(i % 13);
        }
        unsigned k = (unsigned)c17_opaque_int(5);
        g_shr_v(u_out, u_in, c17_opaque_int(35), k);
        for (int i = 0; i < 35; i++) {
            uint32_t exp = (u_in[i] >> k) ^ (u_in[i] << (32u - k));
            C17_CHECK_EQ(u_out[i], exp);
        }
        g_shr_l(u_out, u_in, u_sh, c17_opaque_int(35));
        for (int i = 0; i < 35; i++) {
            C17_CHECK_EQ(u_out[i], u_in[i] >> (u_sh[i] & 15u));
        }
    }

    /* 3. 64-bit unsigned compare + 64-bit multiply + select */
    {
        uint64_t a[21], b[21], out[21];
        for (int i = 0; i < 21; i++) {
            /* Include high-bit values >= 2^63 to catch signed vs unsigned CMP bugs! */
            a[i] = (i & 1) ? (UINT64_C(0x8000000000000000) + (uint64_t)i * 7u) : ((uint64_t)i + 3u);
            b[i] = (i & 2) ? (UINT64_C(0x9000000000000000) + (uint64_t)i * 5u) : ((uint64_t)i * 2u + 1u);
        }
        g_sel_u64(out, a, b, c17_opaque_int(21));
        for (int i = 0; i < 21; i++) {
            uint64_t exp = (a[i] < b[i]) ? (a[i] * b[i]) : (a[i] - b[i]);
            C17_CHECK_EQ(out[i], exp);
        }
    }

    /* 4. Mixed float compare selecting int32 */
    {
        float fa[25], fb[25];
        int32_t ia[25], ib[25], dst[25];
        for (int i = 0; i < 25; i++) {
            fa[i] = (float)(i - 12);
            fb[i] = (float)((i % 5) - 2) * 3.0f;
            ia[i] = 100 + i;
            ib[i] = -200 - i;
        }
        g_sel_fi(dst, fa, fb, ia, ib, c17_opaque_int(25));
        for (int i = 0; i < 25; i++) {
            C17_CHECK_EQ(dst[i], (fa[i] < fb[i]) ? ia[i] : ib[i]);
        }
    }

    /* 5. Multi-reduction + downward loop */
    {
        int32_t arr[33];
        int64_t esum = 0;
        int32_t emn = INT32_MAX, emx = INT32_MIN;
        uint32_t exr = 0u;
        for (int i = 0; i < 33; i++) {
            arr[i] = ((i * 37) % 101) - 50;
            esum += arr[i];
            if (arr[i] < emn) emn = arr[i];
            if (arr[i] > emx) emx = arr[i];
            exr ^= (uint32_t)arr[i];
        }
        int32_t mn = 0, mx = 0;
        uint32_t xr = 0u;
        int64_t s = g_mred(arr, c17_opaque_int(33), &mn, &mx, &xr);
        C17_CHECK_EQ(s, esum);
        C17_CHECK_EQ(mn, emn);
        C17_CHECK_EQ(mx, emx);
        C17_CHECK_EQ(xr, exr);
    }

    C17_END();
}
