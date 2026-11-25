/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Compiled by Clang: verifies true binary ABI compatibility with slimcc-pir
 * for SysV AMD64 mixed-register structs, all-or-nothing GP/XMM exhaustion,
 * partial trailing-byte struct returns, and cross-TU va_list forwarding.
 */
#include <stdarg.h>
#include "interop.h"

struct mix_id clang_ret_mix_id(struct mix_id s, int k) {
    struct mix_id r;
    r.a = s.a + k;
    r.b = s.b * 2.0 + (double)k;
    return r;
}

struct mix_di clang_ret_mix_di(struct mix_di s, double d) {
    struct mix_di r;
    r.a = s.a + d;
    r.b = s.b * 3 + (int)d;
    return r;
}

struct mix_ffi clang_ret_mix_ffi(struct mix_ffi s) {
    struct mix_ffi r;
    r.a = s.a + 1.5f;
    r.b = s.b - 0.5f;
    r.c = s.c + 10;
    return r;
}

struct mix_fif clang_ret_mix_fif(struct mix_fif s) {
    struct mix_fif r;
    r.a = s.a * 2.0f;
    r.b = s.b + 7;
    r.c = s.c + 3.25f;
    return r;
}

struct mix_fd clang_ret_mix_fd(struct mix_fd s) {
    struct mix_fd r;
    r.a = s.a + 2.5f;
    r.b = s.b - 1.25;
    return r;
}

struct odd9 clang_ret_odd9(struct odd9 s) {
    struct odd9 r;
    for (int i = 0; i < 9; ++i) {
        r.b[i] = (unsigned char)(s.b[i] ^ (unsigned char)(0x10 + i));
    }
    return r;
}

long long clang_exhaust_gp5_pair(int g1, int g2, int g3, int g4, int g5,
                                 struct pair_ll p, int tail_r9, int stk_after) {
    return (long long)g1 + g2 * 2 + g3 * 3 + g4 * 4 + g5 * 5
         + p.x * 10 + p.y * 100 + (long long)tail_r9 * 1000 + (long long)stk_after * 10000;
}

double clang_exhaust_fp7_pair(double f1, double f2, double f3, double f4,
                              double f5, double f6, double f7,
                              struct pair_dd p, double tail_xmm7, double stk_after) {
    return f1 + f2 + f3 + f4 + f5 + f6 + f7
         + p.x * 10.0 + p.y * 20.0 + tail_xmm7 * 100.0 + stk_after * 1000.0;
}

double clang_exhaust_mixed_fit(int g1, int g2, int g3, int g4, int g5,
                               double f1, double f2, double f3, double f4,
                               double f5, double f6, double f7,
                               struct mix_id m, int stk_i, double stk_d) {
    return (double)(g1 + g2 + g3 + g4 + g5) + (f1 + f2 + f3 + f4 + f5 + f6 + f7)
         + (double)m.a * 10.0 + m.b * 20.0 + (double)stk_i * 100.0 + stk_d * 1000.0;
}

double clang_exhaust_mixed_spill(int g1, int g2, int g3, int g4, int g5, int g6,
                                 double f1, double f2, double f3, double f4,
                                 double f5, double f6, double f7,
                                 struct mix_id m, double tail_xmm7, struct mem_al16 big) {
    return (double)(g1 + g2 + g3 + g4 + g5 + g6) + (f1 + f2 + f3 + f4 + f5 + f6 + f7)
         + (double)m.a * 10.0 + m.b * 20.0 + tail_xmm7 * 100.0
         + (double)(big.x + big.y * 2 + big.z * 3);
}

double clang_vhelper_consume(int n, va_list ap) {
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
        int iv = va_arg(ap, int);
        double dv = va_arg(ap, double);
        sum += (double)iv + dv * 10.0;
    }
    struct mix_id m = va_arg(ap, struct mix_id);
    struct mix_di d = va_arg(ap, struct mix_di);
    sum += (double)m.a * 100.0 + m.b + d.a + (double)d.b * 100.0;
    return sum;
}

double clang_va_mixed(int tag, ...) {
    va_list ap, cp;
    va_start(ap, tag);
    va_copy(cp, ap);

    double total = (double)tag;
    struct mix_id m1 = va_arg(ap, struct mix_id);
    struct mix_di m2 = va_arg(ap, struct mix_di);
    struct mix_fif m3 = va_arg(ap, struct mix_fif);
    struct mix_fd m4 = va_arg(ap, struct mix_fd);
    union f1_u u1 = va_arg(ap, union f1_u);
    int g5 = va_arg(ap, int);
    struct pair_ll spill_p = va_arg(ap, struct pair_ll);
    int g6_from_r9 = va_arg(ap, int);
    struct mem_al16 big = va_arg(ap, struct mem_al16);
    double f_tail = va_arg(ap, double);

    total += (double)m1.a + m1.b + m2.a + (double)m2.b
           + (double)m3.a + (double)m3.b + (double)m3.c
           + (double)m4.a + m4.b + (double)u1.a
           + (double)g5 + (double)(spill_p.x + spill_p.y)
           + (double)g6_from_r9 + (double)(big.x + big.y + big.z) + f_tail;

    struct mix_id chk = va_arg(cp, struct mix_id);
    total += (double)(chk.a - m1.a) * 1000.0;

    va_end(cp);
    va_end(ap);
    return total;
}

static double call_slimcc_vhelper_via_clang(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double r = slimcc_vhelper_consume(n, ap);
    va_end(ap);
    return r;
}

double clang_calls_slimcc_all(int seed) {
    double acc = 0.0;

    struct mix_id r1 = slimcc_ret_mix_id((struct mix_id){ 11, 2.5 }, 4);
    acc += (double)r1.a + r1.b; /* 24.0 */

    struct mix_di r2 = slimcc_ret_mix_di((struct mix_di){ 3.5, 7 }, 2.0);
    acc += r2.a + (double)r2.b; /* 28.5 */

    struct mix_ffi r3 = slimcc_ret_mix_ffi((struct mix_ffi){ 1.25f, 4.5f, 9 });
    acc += (double)r3.a + (double)r3.b + (double)r3.c; /* 25.75 */

    struct mix_fif r4 = slimcc_ret_mix_fif((struct mix_fif){ 3.5f, 12, 1.75f });
    acc += (double)r4.a + (double)r4.b + (double)r4.c; /* 31.0 */

    struct mix_fd r5 = slimcc_ret_mix_fd((struct mix_fd){ 1.5f, 6.25 });
    acc += (double)r5.a + r5.b; /* 9.0 */

    struct odd9 s6 = { { 1, 2, 3, 4, 5, 6, 7, 8, 9 } };
    struct odd9 r6 = slimcc_ret_odd9(s6);
    int o9sum = 0;
    for (int i = 0; i < 9; ++i) {
        o9sum += (int)r6.b[i];
    }
    acc += (double)o9sum; /* 17+19+17+23+21+23+17+31+25 = 193.0? check exact */

    acc += (double)slimcc_exhaust_gp5_pair(1, 2, 3, 4, 5, (struct pair_ll){ 13, 17 }, 23, 29);
    acc += slimcc_exhaust_fp7_pair(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                   (struct pair_dd){ 2.0, 3.0 }, 8.0, 9.0);
    acc += slimcc_exhaust_mixed_fit(1, 2, 3, 4, 5,
                                    1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                    (struct mix_id){ 3, 4.0 }, 6, 7.0);
    acc += slimcc_exhaust_mixed_spill(1, 2, 3, 4, 5, 6,
                                      1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                      (struct mix_id){ 3, 4.0 }, 8.0,
                                      (struct mem_al16){ 100, 200, 300 });

    union f1_u u1;
    u1.a = 9.0f;
    acc += slimcc_va_mixed(seed,
                           (struct mix_id){ 2, 1.5 },
                           (struct mix_di){ 2.5, 3 },
                           (struct mix_fif){ 4.0f, 5, 6.5f },
                           (struct mix_fd){ 7.5f, 8.0 },
                           u1, 11, (struct pair_ll){ 10, 20 }, 12,
                           (struct mem_al16){ 30, 40, 50 }, 13.5);

    acc += call_slimcc_vhelper_via_clang(2, 10, 2.0, 20, 3.0,
                                         (struct mix_id){ 7, 1.5 },
                                         (struct mix_di){ 2.5, 8 });
    return acc;
}
