/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared header for Round 4 bidirectional slimcc-pir <-> clang ABI interop:
 *   - Unnamed bitfield padding in structs and unions (NO_CLASS / HFA rules)
 *   - Over-aligned _Alignas(32) HFA returns and params (multi_ret_slot align >16B)
 */
#ifndef R4_ABI_EDGE_INTEROP_H
#define R4_ABI_EDGE_INTEROP_H

#include <stdalign.h>

struct ubf_prefix {
    unsigned : 32;
    int x;
};

struct ubf_hi_pad {
    unsigned long long : 64;
    long long y;
};

union u_hfa_bf1 {
    int : 0;
    float f;
};

union u_hfa_bf2 {
    int : 0;
    struct {
        float x;
        float y;
    } p;
};

struct hfa32 {
    _Alignas(32) double a;
    double b;
};

/* Defined in tu2_clang_tu.c (compiled by clang) */
long clang_take_ubf(long a, struct ubf_prefix u4, long b, struct ubf_hi_pad u8, long c);
double clang_take_union_ubf_hfa(union u_hfa_bf1 u1, union u_hfa_bf2 u2, double d);
union u_hfa_bf2 clang_ret_union_ubf_hfa(float x, float y);
struct hfa32 clang_ret_hfa32(double a, double b);
double clang_take_hfa32(struct hfa32 h, double c);

/* Defined in tu1_main.c (compiled by slimcc-pir) */
long slimcc_take_ubf(long a, struct ubf_prefix u4, long b, struct ubf_hi_pad u8, long c);
double slimcc_take_union_ubf_hfa(union u_hfa_bf1 u1, union u_hfa_bf2 u2, double d);
union u_hfa_bf2 slimcc_ret_union_ubf_hfa(float x, float y);
struct hfa32 slimcc_ret_hfa32(double a, double b);
double slimcc_take_hfa32(struct hfa32 h, double c);

int clang_call_slimcc_checks(void);

#endif
