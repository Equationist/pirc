/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared declarations for bidirectional slimcc-pir <-> clang SysV AMD64
 * mixed-register struct ABI, all-or-nothing register exhaustion, and
 * cross-TU va_list array-decay interoperability.
 */
#ifndef INTEROP_SYSV_MIXED_H
#define INTEROP_SYSV_MIXED_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* [INTEGER, SSE] 16B: gp + xmm; return rax + xmm0 */
struct mix_id {
    int a;
    double b;
};

/* [SSE, INTEGER] 16B: xmm + gp; return xmm0 + rax */
struct mix_di {
    double a;
    int b;
};

/* [SSE(v2f32), INTEGER] 12B: xmm + gp; return xmm0 + rax */
struct mix_ffi {
    float a;
    float b;
    int c;
};

/* [INTEGER(float+int merged!), SSE(f32)] 12B: gp + xmm; return rax + xmm0 */
struct mix_fif {
    float a;
    int b;
    float c;
};

/* [SSE, SSE] 16B with 4B interior padding: xmm + xmm */
struct mix_fd {
    float a;
    double b;
};

/* [INTEGER, INTEGER] 16B: 2 GP regs */
struct pair_ll {
    long long x;
    long long y;
};

/* [SSE, SSE] 16B: 2 XMM regs */
struct pair_dd {
    double x;
    double y;
};

/* 9-byte [INTEGER, INTEGER] struct (tests partial trailing GP byte store on return) */
struct odd9 {
    unsigned char b[9];
};

/* 4-byte float union ([SSE(f32)] -> low 32 bits of XMM, never OOB 8B read) */
union f1_u {
    float a;
    float b;
};

/* 32-byte _Alignas(16) MEMORY-class struct (inline on stack, 16B aligned) */
struct mem_al16 {
    _Alignas(16) long long x;
    long long y;
    long long z;
};

/* Direction 1: implemented in Clang, called by slimcc-pir */
struct mix_id clang_ret_mix_id(struct mix_id s, int k);
struct mix_di clang_ret_mix_di(struct mix_di s, double d);
struct mix_ffi clang_ret_mix_ffi(struct mix_ffi s);
struct mix_fif clang_ret_mix_fif(struct mix_fif s);
struct mix_fd clang_ret_mix_fd(struct mix_fd s);
struct odd9 clang_ret_odd9(struct odd9 s);

/* SysV AMD64 all-or-nothing register exhaustion tests */
long long clang_exhaust_gp5_pair(int g1, int g2, int g3, int g4, int g5,
                                 struct pair_ll p, int tail_r9, int stk_after);
double clang_exhaust_fp7_pair(double f1, double f2, double f3, double f4,
                              double f5, double f6, double f7,
                              struct pair_dd p, double tail_xmm7, double stk_after);
double clang_exhaust_mixed_fit(int g1, int g2, int g3, int g4, int g5,
                               double f1, double f2, double f3, double f4,
                               double f5, double f6, double f7,
                               struct mix_id m, int stk_i, double stk_d);
double clang_exhaust_mixed_spill(int g1, int g2, int g3, int g4, int g5, int g6,
                                 double f1, double f2, double f3, double f4,
                                 double f5, double f6, double f7,
                                 struct mix_id m, double tail_xmm7, struct mem_al16 big);

/* Variadic mixed structs + exhaustion + cross-TU va_list forwarding */
double clang_va_mixed(int tag, ...);
double clang_vhelper_consume(int n, va_list ap);
double clang_calls_slimcc_all(int seed);

/* Direction 2: implemented in slimcc-pir, called by Clang */
struct mix_id slimcc_ret_mix_id(struct mix_id s, int k);
struct mix_di slimcc_ret_mix_di(struct mix_di s, double d);
struct mix_ffi slimcc_ret_mix_ffi(struct mix_ffi s);
struct mix_fif slimcc_ret_mix_fif(struct mix_fif s);
struct mix_fd slimcc_ret_mix_fd(struct mix_fd s);
struct odd9 slimcc_ret_odd9(struct odd9 s);

long long slimcc_exhaust_gp5_pair(int g1, int g2, int g3, int g4, int g5,
                                  struct pair_ll p, int tail_r9, int stk_after);
double slimcc_exhaust_fp7_pair(double f1, double f2, double f3, double f4,
                               double f5, double f6, double f7,
                               struct pair_dd p, double tail_xmm7, double stk_after);
double slimcc_exhaust_mixed_fit(int g1, int g2, int g3, int g4, int g5,
                                double f1, double f2, double f3, double f4,
                                double f5, double f6, double f7,
                                struct mix_id m, int stk_i, double stk_d);
double slimcc_exhaust_mixed_spill(int g1, int g2, int g3, int g4, int g5, int g6,
                                  double f1, double f2, double f3, double f4,
                                  double f5, double f6, double f7,
                                  struct mix_id m, double tail_xmm7, struct mem_al16 big);

double slimcc_va_mixed(int tag, ...);
double slimcc_vhelper_consume(int n, va_list ap);

#endif
