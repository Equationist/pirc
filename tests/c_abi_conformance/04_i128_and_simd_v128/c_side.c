/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

static pir_i128 make_i128(int64_t hi, uint64_t lo) {
    return (pir_i128)(((pir_u128)(uint64_t)hi << 64) | (pir_u128)lo);
}

/* Implemented in PIR */
extern pir_i128 pir_add_i128(pir_i128 a, pir_i128 b);
/* DARWIN SPECIFIC: b starts in x1:x2 (odd!), c in x3, d in x4:x5! */
extern pir_i128 pir_darwin_odd_i128(int32_t a, pir_i128 b, int64_t c, pir_i128 d);
/* 7 GP args (x0..x6) + i128: does not fit in x7 alone -> spills to 16B-aligned stack */
extern pir_i128 pir_spill_i128(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                               int64_t a4, int64_t a5, int64_t a6, pir_i128 big);
extern v4f32_t  pir_fma_v4f32(v4f32_t a, v4f32_t b, v4f32_t c);
extern v2f64_t  pir_add_v2f64(v2f64_t a, v2f64_t b);
extern v4i32_t  pir_add_v4i32(v4i32_t a, v4i32_t b);
extern v16u8_t  pir_add_v16i8(v16u8_t a, v16u8_t b);
/* 10 v4f32 args: v0..v7 in regs, v8..v9 on stack */
extern v4f32_t  pir_sum10_v4f32(v4f32_t v0, v4f32_t v1, v4f32_t v2, v4f32_t v3,
                                v4f32_t v4, v4f32_t v5, v4f32_t v6, v4f32_t v7,
                                v4f32_t v8, v4f32_t v9);
extern int32_t  pir_drive_c_i128_simd(void);

/* Implemented in C, called by PIR */
pir_i128 c_darwin_odd_i128(int32_t a, pir_i128 b, int64_t c, pir_i128 d) {
    return (pir_i128)a + b + (pir_i128)c + d;
}

v4f32_t c_sum10_v4f32(v4f32_t v0, v4f32_t v1, v4f32_t v2, v4f32_t v3,
                      v4f32_t v4, v4f32_t v5, v4f32_t v6, v4f32_t v7,
                      v4f32_t v8, v4f32_t v9) {
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9;
}

int main(void) {
    pir_i128 x = make_i128(1, 0xFFFFFFFFFFFFFFFFULL);
    pir_i128 y = make_i128(2, 10);
    pir_i128 sum = pir_add_i128(x, y);
    CHECK_EQ_U64((uint64_t)sum, 9ULL);
    CHECK_EQ_I64((int64_t)((pir_u128)sum >> 64), 4);

    pir_i128 b = make_i128(10, 5);
    pir_i128 d = make_i128(20, 7);
    pir_i128 odd = pir_darwin_odd_i128(3, b, 100, d);
    CHECK_EQ_U64((uint64_t)odd, 115ULL);
    CHECK_EQ_I64((int64_t)((pir_u128)odd >> 64), 30);

    pir_i128 sp = pir_spill_i128(1, 2, 3, 4, 5, 6, 7, make_i128(9, 1000));
    CHECK_EQ_U64((uint64_t)sp, 1028ULL);
    CHECK_EQ_I64((int64_t)((pir_u128)sp >> 64), 9);

    v4f32_t vf = pir_fma_v4f32((v4f32_t){ 1, 2, 3, 4 },
                               (v4f32_t){ 2, 3, 4, 5 },
                               (v4f32_t){ 10, 10, 10, 10 });
    CHECK_EQ_F64(vf[0], 12.0); CHECK_EQ_F64(vf[3], 30.0);

    v2f64_t vd = pir_add_v2f64((v2f64_t){ 1.25, -2.5 }, (v2f64_t){ 3.75, 5.5 });
    CHECK_EQ_F64(vd[0], 5.0); CHECK_EQ_F64(vd[1], 3.0);

    v4i32_t vi = pir_add_v4i32((v4i32_t){ 10, -20, 30, -40 },
                               (v4i32_t){ 1, 2, 3, 4 });
    CHECK_EQ_I64(vi[0], 11); CHECK_EQ_I64(vi[1], -18); CHECK_EQ_I64(vi[3], -36);

    v16u8_t vb = pir_add_v16i8(
        (v16u8_t){ 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 },
        (v16u8_t){ 10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10 });
    CHECK_EQ_I64(vb[0], 11); CHECK_EQ_I64(vb[15], 26);

    v4f32_t v10 = pir_sum10_v4f32(
        (v4f32_t){1,0,0,0}, (v4f32_t){2,0,0,0}, (v4f32_t){3,0,0,0},
        (v4f32_t){4,0,0,0}, (v4f32_t){5,0,0,0}, (v4f32_t){6,0,0,0},
        (v4f32_t){7,0,0,0}, (v4f32_t){8,0,0,0}, (v4f32_t){9,1,2,3},
        (v4f32_t){10,4,5,6});
    CHECK_EQ_F64(v10[0], 55.0); CHECK_EQ_F64(v10[1], 5.0); CHECK_EQ_F64(v10[3], 9.0);

    CHECK_EQ_I64(pir_drive_c_i128_simd(), 1);
    printf("PASS\n");
    return 0;
}
