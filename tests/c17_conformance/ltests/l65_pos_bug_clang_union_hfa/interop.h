/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared header for bidirectional slimcc-pir <-> clang AAPCS64 union-HFA,
 * over-aligned HFA, and stack-spilled small struct ABI interoperability.
 */
#ifndef L65_UNION_HFA_INTEROP_H
#define L65_UNION_HFA_INTEROP_H

union hfa_f1 {
    float a;
    float b;
};

union hfa_f2 {
    float arr[2];
    struct { float x, y; } p;
};

union hfa_f2_mix {
    float single;
    struct { float x, y; } pair;
};

union hfa_d3 {
    double arr[3];
    struct { double x, y; } p;
};

struct with_union_hfa {
    float z;
    union hfa_f2_mix u;
};

union non_hfa_fi {
    float f;
    int i;
};

struct over_hfa16 {
    _Alignas(16) float a;
    float b;
    float c;
    float d;
};

struct stk_s3 {
    signed char a, b, c;
};

struct stk_s12 {
    int a, b, c;
};

/* Defined in Clang TU, called from slimcc-pir TU */
double clang_consume_union_hfas(union hfa_f1 u1, union hfa_f2 u2,
                                union hfa_f2_mix u3, union hfa_d3 u4,
                                union non_hfa_fi u5);
float clang_consume_nested_and_over(struct with_union_hfa w, struct over_hfa16 o);
union hfa_f2 clang_return_union_hfa(float x, float y);
struct over_hfa16 clang_return_over_hfa(float a, float b, float c, float d);
int clang_consume_spilled_structs(long g0, long g1, long g2, long g3,
                                  long g4, long g5, long g6, long g7,
                                  struct stk_s3 s1, struct stk_s3 s2,
                                  struct stk_s12 s3);

/* Defined in slimcc-pir TU, called from Clang TU */
double slimcc_consume_union_hfas(union hfa_f1 u1, union hfa_f2 u2,
                                 union hfa_f2_mix u3, union hfa_d3 u4,
                                 union non_hfa_fi u5);
float slimcc_consume_nested_and_over(struct with_union_hfa w, struct over_hfa16 o);
union hfa_f2 slimcc_return_union_hfa(float x, float y);
struct over_hfa16 slimcc_return_over_hfa(float a, float b, float c, float d);
int slimcc_consume_spilled_structs(long g0, long g1, long g2, long g3,
                                   long g4, long g5, long g6, long g7,
                                   struct stk_s3 s1, struct stk_s3 s2,
                                   struct stk_s12 s3);

/* Defined in Clang TU: calls all slimcc-pir functions and checks results */
int clang_verify_slimcc_union_hfas(void);

#endif /* L65_UNION_HFA_INTEROP_H */
