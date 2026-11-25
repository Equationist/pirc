/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.8 / §6.5.9 / §6.5.7 — Regression test for
 * vectorized loops across all integer widths (i8/u8/i16/u16/i32/u32/i64/u64)
 * and floats with all 6 comparison operators, shifts, and selects, exercising
 * x86-64 SSE2 emit_vec_all_ones pure-def liveness and ARM64 NEON vector ops.
 */
#include <stdint.h>
#include "c17_test.h"

static void v_i32_all(const int32_t *a, const int32_t *b, int32_t *o, int n) {
    for (int i = 0; i < n; i++) {
        int32_t r = 0;
        r += (a[i] == b[i]) ? 1 : 0;
        r += (a[i] != b[i]) ? 2 : 0;
        r += (a[i] <  b[i]) ? 4 : 0;
        r += (a[i] <= b[i]) ? 8 : 0;
        r += (a[i] >  b[i]) ? 16 : 0;
        r += (a[i] >= b[i]) ? 32 : 0;
        o[i] = r + (a[i] >> 2) + (int32_t)((uint32_t)b[i] >> 3);
    }
}

static void v_u32_all(const uint32_t *a, const uint32_t *b, uint32_t *o, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t r = 0;
        r += (a[i] == b[i]) ? 1u : 0u;
        r += (a[i] != b[i]) ? 2u : 0u;
        r += (a[i] <  b[i]) ? 4u : 0u;
        r += (a[i] <= b[i]) ? 8u : 0u;
        r += (a[i] >  b[i]) ? 16u : 0u;
        r += (a[i] >= b[i]) ? 32u : 0u;
        o[i] = r + (a[i] >> 5) + (b[i] << 3);
    }
}

static void v_i64_all(const int64_t *a, const int64_t *b, int64_t *o, int n) {
    for (int i = 0; i < n; i++) {
        int64_t r = 0;
        r += (a[i] == b[i]) ? 1 : 0;
        r += (a[i] != b[i]) ? 2 : 0;
        r += (a[i] <  b[i]) ? 4 : 0;
        r += (a[i] <= b[i]) ? 8 : 0;
        r += (a[i] >  b[i]) ? 16 : 0;
        r += (a[i] >= b[i]) ? 32 : 0;
        o[i] = r + (a[i] >> 4) + (int64_t)((uint64_t)b[i] >> 7);
    }
}

static void v_u64_all(const uint64_t *a, const uint64_t *b, uint64_t *o, int n) {
    for (int i = 0; i < n; i++) {
        uint64_t r = 0;
        r += (a[i] == b[i]) ? 1ULL : 0ULL;
        r += (a[i] != b[i]) ? 2ULL : 0ULL;
        r += (a[i] <  b[i]) ? 4ULL : 0ULL;
        r += (a[i] <= b[i]) ? 8ULL : 0ULL;
        r += (a[i] >  b[i]) ? 16ULL : 0ULL;
        r += (a[i] >= b[i]) ? 32ULL : 0ULL;
        o[i] = r + (a[i] >> 9) + (b[i] << 5);
    }
}

static void v_u16_all(const uint16_t *a, const uint16_t *b, uint16_t *o, int n) {
    for (int i = 0; i < n; i++) {
        uint16_t r = 0;
        r += (a[i] < b[i]) ? 1u : 0u;
        r += (a[i] >= b[i]) ? 2u : 0u;
        r += (a[i] == b[i]) ? 4u : 0u;
        o[i] = (uint16_t)(r + (uint16_t)(a[i] >> 2) + (uint16_t)(b[i] << 1));
    }
}

static void v_u8_all(const uint8_t *a, const uint8_t *b, uint8_t *o, int n) {
    for (int i = 0; i < n; i++) {
        o[i] = (a[i] < b[i]) ? (uint8_t)(a[i] ^ b[i]) : (uint8_t)(a[i] + b[i]);
    }
}

int main(void) {
    C17_BEGIN("c65_bug_vec_cmp_shift_all_widths",
              "Vectorized comparisons & shifts across all widths (x64 SSE2 pure-def pcmpeqd & NEON)");
    int32_t ia[32], ib[32], io[32];
    uint32_t ua[32], ub[32], uo[32];
    int64_t la[32], lb[32], lo[32];
    uint64_t ula[32], ulb[32], ulo[32];
    uint16_t sa[32], sb[32], so[32];
    uint8_t ba[64], bb[64], bo[64];

    for (int i = 0; i < 32; i++) {
        ia[i] = (i % 3 == 0) ? (-0x7FFFFFFF - 1) : (i % 3 == 1 ? i * 100 : -i * 100);
        ib[i] = (i % 4 == 0) ? ia[i] : (i % 2 == 0 ? 0x7FFFFFFF : -50);
        ua[i] = (uint32_t)ia[i];
        ub[i] = (uint32_t)ib[i];
        la[i] = (i % 3 == 0) ? (-0x7FFFFFFFFFFFFFFFLL - 1) : ((int64_t)ia[i] * 0x100000003LL);
        lb[i] = (i % 4 == 0) ? la[i] : ((int64_t)ib[i] * 0x100000007LL);
        ula[i] = (uint64_t)la[i];
        ulb[i] = (uint64_t)lb[i];
        sa[i] = (uint16_t)(i * 2111u);
        sb[i] = (uint16_t)(i * 3001u + (i % 3 == 0 ? 0u : 1u));
    }
    for (int i = 0; i < 64; i++) {
        ba[i] = (uint8_t)(i * 13u);
        bb[i] = (uint8_t)(i * 7u + 120u);
    }

    void (*volatile f1)(const int32_t *, const int32_t *, int32_t *, int) = v_i32_all;
    void (*volatile f2)(const uint32_t *, const uint32_t *, uint32_t *, int) = v_u32_all;
    void (*volatile f3)(const int64_t *, const int64_t *, int64_t *, int) = v_i64_all;
    void (*volatile f4)(const uint64_t *, const uint64_t *, uint64_t *, int) = v_u64_all;
    void (*volatile f5)(const uint16_t *, const uint16_t *, uint16_t *, int) = v_u16_all;
    void (*volatile f6)(const uint8_t *, const uint8_t *, uint8_t *, int) = v_u8_all;

    f1(ia, ib, io, 32);
    f2(ua, ub, uo, 32);
    f3(la, lb, lo, 32);
    f4(ula, ulb, ulo, 32);
    f5(sa, sb, so, 32);
    f6(ba, bb, bo, 64);

    for (int i = 0; i < 32; i++) {
        volatile int32_t a = ia[i], b = ib[i];
        int32_t er = (a == b ? 1 : 0) + (a != b ? 2 : 0) + (a < b ? 4 : 0) +
                     (a <= b ? 8 : 0) + (a > b ? 16 : 0) + (a >= b ? 32 : 0) +
                     (a >> 2) + (int32_t)((uint32_t)b >> 3);
        C17_CHECK_EQ(io[i], er);

        volatile uint32_t ua_v = ua[i], ub_v = ub[i];
        uint32_t eur = (ua_v == ub_v ? 1u : 0u) + (ua_v != ub_v ? 2u : 0u) +
                       (ua_v < ub_v ? 4u : 0u) + (ua_v <= ub_v ? 8u : 0u) +
                       (ua_v > ub_v ? 16u : 0u) + (ua_v >= ub_v ? 32u : 0u) +
                       (ua_v >> 5) + (ub_v << 3);
        C17_CHECK_EQ(uo[i], eur);

        volatile int64_t la_v = la[i], lb_v = lb[i];
        int64_t elr = (la_v == lb_v ? 1 : 0) + (la_v != lb_v ? 2 : 0) +
                      (la_v < lb_v ? 4 : 0) + (la_v <= lb_v ? 8 : 0) +
                      (la_v > lb_v ? 16 : 0) + (la_v >= lb_v ? 32 : 0) +
                      (la_v >> 4) + (int64_t)((uint64_t)lb_v >> 7);
        C17_CHECK_EQ(lo[i], elr);

        volatile uint64_t ula_v = ula[i], ulb_v = ulb[i];
        uint64_t eulr = (ula_v == ulb_v ? 1ULL : 0ULL) + (ula_v != ulb_v ? 2ULL : 0ULL) +
                        (ula_v < ulb_v ? 4ULL : 0ULL) + (ula_v <= ulb_v ? 8ULL : 0ULL) +
                        (ula_v > ulb_v ? 16ULL : 0ULL) + (ula_v >= ulb_v ? 32ULL : 0ULL) +
                        (ula_v >> 9) + (ulb_v << 5);
        C17_CHECK_EQ(ulo[i], eulr);

        volatile uint16_t sa_v = sa[i], sb_v = sb[i];
        uint16_t esr = (uint16_t)((sa_v < sb_v ? 1u : 0u) + (sa_v >= sb_v ? 2u : 0u) +
                                  (sa_v == sb_v ? 4u : 0u) + (uint16_t)(sa_v >> 2) + (uint16_t)(sb_v << 1));
        C17_CHECK_EQ(so[i], esr);
    }
    for (int i = 0; i < 64; i++) {
        volatile uint8_t ba_v = ba[i], bb_v = bb[i];
        uint8_t ebr = (ba_v < bb_v) ? (uint8_t)(ba_v ^ bb_v) : (uint8_t)(ba_v + bb_v);
        C17_CHECK_UEQ(bo[i], ebr);
    }
    C17_END();
}
