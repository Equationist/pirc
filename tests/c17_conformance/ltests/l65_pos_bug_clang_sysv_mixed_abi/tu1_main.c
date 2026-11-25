/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 / §7.16 — Bidirectional slimcc-pir <-> clang
 * SysV AMD64 mixed-register struct ABI, all-or-nothing register exhaustion,
 * partial trailing-byte struct returns, and cross-TU va_list forwarding.
 */
#include <stdarg.h>
#include "c17_test.h"
#include "interop.h"

struct mix_id slimcc_ret_mix_id(struct mix_id s, int k) {
    struct mix_id r;
    r.a = s.a + k;
    r.b = s.b * 2.0 + (double)k;
    return r;
}

struct mix_di slimcc_ret_mix_di(struct mix_di s, double d) {
    struct mix_di r;
    r.a = s.a + d;
    r.b = s.b * 3 + (int)d;
    return r;
}

struct mix_ffi slimcc_ret_mix_ffi(struct mix_ffi s) {
    struct mix_ffi r;
    r.a = s.a + 1.5f;
    r.b = s.b - 0.5f;
    r.c = s.c + 10;
    return r;
}

struct mix_fif slimcc_ret_mix_fif(struct mix_fif s) {
    struct mix_fif r;
    r.a = s.a * 2.0f;
    r.b = s.b + 7;
    r.c = s.c + 3.25f;
    return r;
}

struct mix_fd slimcc_ret_mix_fd(struct mix_fd s) {
    struct mix_fd r;
    r.a = s.a + 2.5f;
    r.b = s.b - 1.25;
    return r;
}

struct odd9 slimcc_ret_odd9(struct odd9 s) {
    struct odd9 r;
    for (int i = 0; i < 9; ++i) {
        r.b[i] = (unsigned char)(s.b[i] ^ (unsigned char)(0x10 + i));
    }
    return r;
}

long long slimcc_exhaust_gp5_pair(int g1, int g2, int g3, int g4, int g5,
                                  struct pair_ll p, int tail_r9, int stk_after) {
    return (long long)g1 + g2 * 2 + g3 * 3 + g4 * 4 + g5 * 5
         + p.x * 10 + p.y * 100 + (long long)tail_r9 * 1000 + (long long)stk_after * 10000;
}

double slimcc_exhaust_fp7_pair(double f1, double f2, double f3, double f4,
                               double f5, double f6, double f7,
                               struct pair_dd p, double tail_xmm7, double stk_after) {
    return f1 + f2 + f3 + f4 + f5 + f6 + f7
         + p.x * 10.0 + p.y * 20.0 + tail_xmm7 * 100.0 + stk_after * 1000.0;
}

double slimcc_exhaust_mixed_fit(int g1, int g2, int g3, int g4, int g5,
                                double f1, double f2, double f3, double f4,
                                double f5, double f6, double f7,
                                struct mix_id m, int stk_i, double stk_d) {
    return (double)(g1 + g2 + g3 + g4 + g5) + (f1 + f2 + f3 + f4 + f5 + f6 + f7)
         + (double)m.a * 10.0 + m.b * 20.0 + (double)stk_i * 100.0 + stk_d * 1000.0;
}

double slimcc_exhaust_mixed_spill(int g1, int g2, int g3, int g4, int g5, int g6,
                                  double f1, double f2, double f3, double f4,
                                  double f5, double f6, double f7,
                                  struct mix_id m, double tail_xmm7, struct mem_al16 big) {
    return (double)(g1 + g2 + g3 + g4 + g5 + g6) + (f1 + f2 + f3 + f4 + f5 + f6 + f7)
         + (double)m.a * 10.0 + m.b * 20.0 + tail_xmm7 * 100.0
         + (double)(big.x + big.y * 2 + big.z * 3);
}

double slimcc_vhelper_consume(int n, va_list ap) {
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

double slimcc_va_mixed(int tag, ...) {
    va_list ap, cp;
    va_start(ap, tag);
    va_copy(cp, ap);

    double total = (double)tag;
    struct mix_id m1 = va_arg(ap, struct mix_id);
    struct mix_di m2 = va_arg(ap, struct mix_di);
    struct mix_fif m3 = va_arg(ap, struct mix_fif);
    struct mix_fd m4 = va_arg(ap, struct mix_fd);
    union f1_u u1 = va_arg(ap, union f1_u);
    /* Exhaust remaining GP (now at 1+1+1+1=4 GP used -> 5th GP) and XMM */
    int g5 = va_arg(ap, int);
    struct pair_ll spill_p = va_arg(ap, struct pair_ll); /* 5+2 > 6 -> spills to overflow! */
    int g6_from_r9 = va_arg(ap, int);                    /* still comes from r9! */
    struct mem_al16 big = va_arg(ap, struct mem_al16);   /* MEMORY class, 16B aligned! */
    double f_tail = va_arg(ap, double);

    total += (double)m1.a + m1.b + m2.a + (double)m2.b
           + (double)m3.a + (double)m3.b + (double)m3.c
           + (double)m4.a + m4.b + (double)u1.a
           + (double)g5 + (double)(spill_p.x + spill_p.y)
           + (double)g6_from_r9 + (double)(big.x + big.y + big.z) + f_tail;

    /* Verify va_copy cursor matches first struct independently */
    struct mix_id chk = va_arg(cp, struct mix_id);
    total += (double)(chk.a - m1.a) * 1000.0;

    va_end(cp);
    va_end(ap);
    return total;
}

static double call_clang_vhelper_via_slimcc(int n, ...) {
    va_list ap;
    va_start(ap, n);
    double r = clang_vhelper_consume(n, ap);
    va_end(ap);
    return r;
}

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_sysv_mixed_abi",
              "Bidirectional slimcc-pir <-> clang SysV AMD64 mixed struct & exhaustion ABI");

    /* Direction 1: slimcc-pir -> Clang mixed struct pass & return */
    {
        struct mix_id s1 = { 11, 2.5 };
        struct mix_id r1 = clang_ret_mix_id(s1, c17_opaque_int(4));
        C17_CHECK_EQ(r1.a, 15);
        C17_CHECK_DBL(r1.b, 9.0, 1e-12);

        struct mix_di s2 = { 3.5, 7 };
        struct mix_di r2 = clang_ret_mix_di(s2, c17_opaque_dbl(2.0));
        C17_CHECK_DBL(r2.a, 5.5, 1e-12);
        C17_CHECK_EQ(r2.b, 23);

        struct mix_ffi s3 = { 1.25f, 4.5f, 9 };
        struct mix_ffi r3 = clang_ret_mix_ffi(s3);
        C17_CHECK_DBL((double)r3.a, 2.75, 1e-6);
        C17_CHECK_DBL((double)r3.b, 4.0, 1e-6);
        C17_CHECK_EQ(r3.c, 19);

        struct mix_fif s4 = { 3.5f, 12, 1.75f };
        struct mix_fif r4 = clang_ret_mix_fif(s4);
        C17_CHECK_DBL((double)r4.a, 7.0, 1e-6);
        C17_CHECK_EQ(r4.b, 19);
        C17_CHECK_DBL((double)r4.c, 5.0, 1e-6);

        struct mix_fd s5 = { 1.5f, 6.25 };
        struct mix_fd r5 = clang_ret_mix_fd(s5);
        C17_CHECK_DBL((double)r5.a, 4.0, 1e-6);
        C17_CHECK_DBL(r5.b, 5.0, 1e-12);

        struct odd9 s6 = { { 1, 2, 3, 4, 5, 6, 7, 8, 9 } };
        struct odd9 r6 = clang_ret_odd9(s6);
        for (int i = 0; i < 9; ++i) {
            C17_CHECK_EQ(r6.b[i], (int)((unsigned char)((i + 1) ^ (0x10 + i))));
        }
    }

    /* Direction 1: SysV all-or-nothing GP & XMM exhaustion */
    {
        struct pair_ll p = { 13, 17 };
        long long r = clang_exhaust_gp5_pair(1, 2, 3, 4, 5, p, 23, 29);
        /* 1+4+9+16+25 + 130+1700 + 23000 + 290000 = 314885 */
        C17_CHECK_EQ(r, 314885LL);

        struct pair_dd dp = { 2.0, 3.0 };
        double rd = clang_exhaust_fp7_pair(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, dp, 8.0, 9.0);
        /* 28 + 20 + 60 + 800 + 9000 = 9908 */
        C17_CHECK_DBL(rd, 9908.0, 1e-12);

        struct mix_id m = { 3, 4.0 };
        double rm1 = clang_exhaust_mixed_fit(1, 2, 3, 4, 5,
                                             1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                             m, 6, 7.0);
        /* 15 + 28 + 30 + 80 + 600 + 7000 = 7753 */
        C17_CHECK_DBL(rm1, 7753.0, 1e-12);

        struct mem_al16 big = { 100, 200, 300 };
        double rm2 = clang_exhaust_mixed_spill(1, 2, 3, 4, 5, 6,
                                               1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                                               m, 8.0, big);
        /* 21 + 28 + 30 + 80 + 800 + (100+400+900) = 2359 */
        C17_CHECK_DBL(rm2, 2359.0, 1e-12);
    }

    /* Direction 1: Variadic mixed structs + exhaustion + cross-TU va_list forwarding */
    {
        struct mix_id m1 = { 2, 1.5 };
        struct mix_di m2 = { 2.5, 3 };
        struct mix_fif m3 = { 4.0f, 5, 6.5f };
        struct mix_fd m4 = { 7.5f, 8.0 };
        union f1_u u1;
        u1.a = 9.0f;
        struct pair_ll sp = { 10, 20 };
        struct mem_al16 big = { 30, 40, 50 };
        double rv = clang_va_mixed(1, m1, m2, m3, m4, u1, 11, sp, 12, big, 13.5);
        /* 1 + 2+1.5 + 2.5+3 + 4+5+6.5 + 7.5+8 + 9 + 11 + 30 + 12 + 120 + 13.5 = 236.5 */
        C17_CHECK_DBL(rv, 236.5, 1e-9);

        struct mix_id vm = { 7, 1.5 };
        struct mix_di vd = { 2.5, 8 };
        double vh = call_clang_vhelper_via_slimcc(2, 10, 2.0, 20, 3.0, vm, vd);
        /* (10+20) + (20+30) + 700+1.5+2.5+800 = 1584.0 */
        C17_CHECK_DBL(vh, 1584.0, 1e-9);
    }

    /* Direction 2: Clang caller -> slimcc-pir callees (all categories) */
    {
        double r2 = clang_calls_slimcc_all(c17_opaque_int(1));
        /* Exact deterministic checksum verified against Clang reference run */
        C17_CHECK_DBL(r2, 337020.75, 1e-6);
    }

    C17_END();
}
