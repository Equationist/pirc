/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common.h"

/* Implemented in PIR */
extern int64_t pir_many_args(
    int64_t g0, int64_t g1, int64_t g2, int64_t g3,
    int64_t g4, int64_t g5, int64_t g6, int64_t g7,
    int64_t g8, int64_t g9, int64_t g10, int64_t g11,
    double f0, double f1, double f2, double f3,
    double f4, double f5, double f6, double f7,
    double f8, double f9, double f10, double f11);

/* DARWIN SPECIFIC: after 8 GP regs (x0..x7), sub-8-byte stack args pack tightly
 * at natural alignments: s0(off 0, 1B), s1(off 1, 1B), s2(off 2, 2B),
 * s3(off 4, 4B), s4(off 8, 1B), pad(7B), s5(off 16, 8B). */
extern int64_t pir_darwin_packed_stack(
    int64_t g0, int64_t g1, int64_t g2, int64_t g3,
    int64_t g4, int64_t g5, int64_t g6, int64_t g7,
    int8_t s0, uint8_t s1, int16_t s2, int32_t s3, int8_t s4, int64_t s5);

extern int32_t pir_drive_c_packed_stack(void);

/* Implemented in C, called by PIR */
int64_t c_darwin_packed_stack(
    int64_t g0, int64_t g1, int64_t g2, int64_t g3,
    int64_t g4, int64_t g5, int64_t g6, int64_t g7,
    int8_t s0, uint8_t s1, int16_t s2, int32_t s3, int8_t s4, int64_t s5) {
    return g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7
           + (int64_t)s0 + (int64_t)s1 + (int64_t)s2
           + (int64_t)s3 + (int64_t)s4 + s5;
}

int main(void) {
    /* Sum 1..12 = 78; double sum 10..120 step 10 = 780 -> total = 858 */
    int64_t m = pir_many_args(
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0,
        90.0, 100.0, 110.0, 120.0);
    CHECK_EQ_I64(m, 858);

    /* 1+2+3+4+5+6+7+8 = 36; (-5) + 250 + (-1000) + 50000 + (-3) + 1000000 = 1049242 -> 1049278 */
    int64_t p = pir_darwin_packed_stack(
        1, 2, 3, 4, 5, 6, 7, 8,
        (int8_t)-5, (uint8_t)250, (int16_t)-1000, (int32_t)50000,
        (int8_t)-3, 1000000LL);
    CHECK_EQ_I64(p, 1049278);

    CHECK_EQ_I64(pir_drive_c_packed_stack(), 1);
    printf("PASS\n");
    return 0;
}
