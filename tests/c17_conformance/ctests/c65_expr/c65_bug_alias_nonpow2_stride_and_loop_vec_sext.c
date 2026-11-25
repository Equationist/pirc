/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.2.5 / §6.5.6 — Regression tests for:
 *   1. Non-power-of-2 modular stride wraparound in alias analysis (DSE
 *      store-forwarding, dead-store elimination, load CSE, and LICM hoisting).
 *   2. Unsigned i32 induction variable with signed (int32_t) array indexing
 *      in the loop vectorizer (SEXT vs ZEXT signedness mismatch).
 */
#include <stdint.h>
#include "c17_test.h"

/* --- Part 1: Non-power-of-2 wrapping stride alias disambiguation --- */
static volatile uint64_t g_idx12 = 0x1555555555555556ULL; /* * 12 == 8  (mod 2^64) */
static volatile uint64_t g_idx56 = 0x0492492492492493ULL; /* * 56 == 40 (mod 2^64) */
static volatile uint64_t g_idx3  = 0x5555555555555556ULL; /* * 3  == 2  (mod 2^64) */

static uint32_t dse_forward_stride12(uint8_t *buf, uint64_t idx) {
    uint32_t *p_const = (uint32_t *)(buf + 8);
    uint32_t *p_var   = (uint32_t *)(buf + idx * 12ULL);
    *p_const = 0x11111111u;
    *p_var   = 0x22222222u;
    return *p_const;
}

static uint32_t dse_dead_store_stride12(uint8_t *buf, uint64_t idx) {
    uint32_t *p_const = (uint32_t *)(buf + 8);
    uint32_t *p_var   = (uint32_t *)(buf + idx * 12ULL);
    *p_const = 0x12345678u;
    uint32_t observed = *p_var;
    *p_const = 0x99999999u;
    return observed;
}

static uint32_t dse_load_cse_stride56(uint8_t *buf, uint64_t idx) {
    uint32_t *p_const = (uint32_t *)(buf + 40);
    uint32_t *p_var   = (uint32_t *)(buf + idx * 56ULL);
    uint32_t before = *p_const;
    *p_var = 0xCAFEBABEu;
    uint32_t after = *p_const;
    return before ^ after;
}

static uint32_t licm_hoist_stride12(uint8_t *buf, uint64_t idx, int n) {
    uint32_t *p_const = (uint32_t *)(buf + 8);
    uint32_t *p_var   = (uint32_t *)(buf + idx * 12ULL);
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) {
        sum += *p_const;
        *p_var = (uint32_t)(i + 10);
    }
    return sum;
}

static uint8_t dse_forward_stride3(uint8_t *buf, uint64_t idx) {
    uint8_t *p_const = buf + 2;
    uint8_t *p_var   = buf + idx * 3ULL;
    *p_const = 0x11u;
    *p_var   = 0x77u;
    return *p_const;
}

static uint32_t (*volatile fp_fwd12)(uint8_t *, uint64_t)       = dse_forward_stride12;
static uint32_t (*volatile fp_dead12)(uint8_t *, uint64_t)      = dse_dead_store_stride12;
static uint32_t (*volatile fp_cse56)(uint8_t *, uint64_t)       = dse_load_cse_stride56;
static uint32_t (*volatile fp_licm12)(uint8_t *, uint64_t, int) = licm_hoist_stride12;
static uint8_t  (*volatile fp_fwd3)(uint8_t *, uint64_t)        = dse_forward_stride3;

/* --- Part 2: Unsigned u32 IV with signed (int32_t) index extension --- */
static void vec_u32_iv_sext_rmw(int32_t *mid, uint32_t start, uint32_t end) {
    for (uint32_t i = start; i < end; i++) {
        int32_t si = (int32_t)i;
        mid[si] = mid[si] + 7;
    }
}

static void vec_u32_iv_sext_copy(int32_t *dst_mid, const int32_t *src_mid,
                                 uint32_t start, uint32_t end) {
    for (uint32_t i = start; i < end; i++) {
        int32_t si = (int32_t)i;
        dst_mid[si] = src_mid[si] * 3 + 1;
    }
}

static void (*volatile fp_rmw)(int32_t *, uint32_t, uint32_t) = vec_u32_iv_sext_rmw;
static void (*volatile fp_cpy)(int32_t *, const int32_t *, uint32_t, uint32_t) = vec_u32_iv_sext_copy;

int main(void) {
    C17_BEGIN("c65_bug_alias_nonpow2_stride_and_loop_vec_sext",
              "Non-pow2 wrapping stride aliasing and loop_vec u32 IV SEXT/ZEXT mismatch");

    uint8_t buf[64] = {0};
    uint64_t i12 = g_idx12;
    uint64_t i56 = g_idx56;
    uint64_t i3  = g_idx3;

    C17_CHECK_EQ(fp_fwd12(buf, i12), 0x22222222u);
    C17_CHECK_EQ(fp_dead12(buf, i12), 0x12345678u);

    *(uint32_t *)(buf + 40) = 0x01020304u;
    C17_CHECK_EQ(fp_cse56(buf, i56), 0x01020304u ^ 0xCAFEBABEu);

    *(uint32_t *)(buf + 8) = 1u;
    C17_CHECK_EQ(fp_licm12(buf, i12, 5), 47u);
    C17_CHECK_EQ(fp_fwd3(buf, i3), 0x77u);

    int32_t vbuf[128];
    int32_t vdst[128];
    for (int i = 0; i < 128; i++) {
        vbuf[i] = i;
        vdst[i] = 0;
    }
    fp_rmw(&vbuf[80], 0xFFFFFFC0u, 0xFFFFFFF0u);
    fp_cpy(&vdst[80], &vbuf[80], 0xFFFFFFC0u, 0xFFFFFFF0u);

    int ok = 1;
    for (int idx = 0; idx < 128; idx++) {
        int32_t exp_b = (idx >= 16 && idx < 64) ? (idx + 7) : idx;
        int32_t exp_d = (idx >= 16 && idx < 64) ? (exp_b * 3 + 1) : 0;
        if (vbuf[idx] != exp_b || vdst[idx] != exp_d) {
            ok = 0;
            break;
        }
    }
    C17_CHECK(ok);

    C17_END();
}
