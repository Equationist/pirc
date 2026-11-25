/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 / §6.7.2.1 / §7.16 — Bidirectional
 * slimcc-pir <-> clang ABI interop for AAPCS64 zero-width bitfield HFAs,
 * SysV AMD64 unnamed bitfields, and SysV AMD64 NO_CLASS eightbytes.
 */
#include <stdarg.h>
#include "c17_test.h"
#include "interop.h"

S_f_z0_f slimcc_f_z0_f(int k, S_f_z0_f v, float f, int t) {
    return (S_f_z0_f){ v.a + f + (float)k, v.b + (float)t };
}
S_z0_ff slimcc_z0_ff(int k, S_z0_ff v, float f, int t) {
    return (S_z0_ff){ v.a + f + (float)k, v.b + (float)t };
}
S_ff_z0 slimcc_ff_z0(int k, S_ff_z0 v, float f, int t) {
    return (S_ff_z0){ v.a + f + (float)k, v.b + (float)t };
}
S_d_z0_d slimcc_d_z0_d(int k, S_d_z0_d v, double d, int t) {
    return (S_d_z0_d){ v.a + d + (double)k, v.b + (double)t };
}
U_z0_f slimcc_u_z0_f(int k, U_z0_f v, float f, int t) {
    U_z0_f r;
    r.a = v.a + f + (float)(k + t);
    return r;
}

BF_f_u32 slimcc_bf_f_u32(int k, BF_f_u32 v, double d, int t) {
    return (BF_f_u32){ v.a + (float)d + (float)(k + t) };
}
BF_u32_f slimcc_bf_u32_f(int k, BF_u32_f v, double d, int t) {
    return (BF_u32_f){ v.a + (float)d + (float)(k + t) };
}
BF_u64_d slimcc_bf_u64_d(int k, BF_u64_d v, double d, int t) {
    return (BF_u64_d){ v.d + d + (double)(k + t) };
}
BF_d_u32 slimcc_bf_d_u32(int k, BF_d_u32 v, double d, int t) {
    return (BF_d_u32){ v.d + d + (double)(k + t) };
}
BF_f_u1_f slimcc_bf_f_u1_f(int k, BF_f_u1_f v, float f, int t) {
    return (BF_f_u1_f){ v.a + f + (float)k, v.b + (float)t };
}
BF_u32_ff slimcc_bf_u32_ff(int k, BF_u32_ff v, float f, int t) {
    return (BF_u32_ff){ v.a + f + (float)k, v.b + (float)t };
}

A16_i slimcc_a16_i(int x, A16_i s, int y, double z) {
    return (A16_i){ x + s.a + y + (int)z };
}
A16_d slimcc_a16_d(int x, A16_d s, int y, double z) {
    return (A16_d){ s.a + z + (double)(x + y) };
}
A16_f slimcc_a16_f(int x, A16_f s, int y, double z) {
    return (A16_f){ s.a + (float)z + (float)(x + y) };
}
A16_c slimcc_a16_c(int x, A16_c s, int y) {
    return (A16_c){ (char)(s.c + x + y) };
}
U16_fd slimcc_u16_fd(int x, U16_fd u, double d, int y) {
    U16_fd r;
    r.b = u.b + d + (double)(x + y);
    return r;
}
int slimcc_va_a16_i(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int p = va_arg(ap, int);
    A16_i x = va_arg(ap, A16_i);
    int t = va_arg(ap, int);
    va_end(ap);
    return tag + p * 10 + x.a * 100 + t * 1000;
}
double slimcc_va_a16_d(int tag, ...) {
    va_list ap;
    va_start(ap, tag);
    int p = va_arg(ap, int);
    A16_d x = va_arg(ap, A16_d);
    int t = va_arg(ap, int);
    va_end(ap);
    return (double)tag + (double)p * 10.0 + x.a * 100.0 + (double)t * 1000.0;
}

int main(void) {
    C17_BEGIN("l65_pos_bug_clang_bitfield_noclass_abi",
              "Bidirectional slimcc-pir <-> clang ABI for :0 HFA, unnamed bitfields, & NO_CLASS eightbytes");

    /* slimcc-pir caller -> clang callee */
    S_f_z0_f h1 = clang_f_z0_f(1, (S_f_z0_f){ 2.0f, 3.0f }, 4.5f, 5);
    C17_CHECK(h1.a == 7.5f && h1.b == 8.0f);
    S_z0_ff h2 = clang_z0_ff(1, (S_z0_ff){ 2.0f, 3.0f }, 4.5f, 5);
    C17_CHECK(h2.a == 7.5f && h2.b == 8.0f);
    S_ff_z0 h3 = clang_ff_z0(1, (S_ff_z0){ 2.0f, 3.0f }, 4.5f, 5);
    C17_CHECK(h3.a == 7.5f && h3.b == 8.0f);
    S_d_z0_d h4 = clang_d_z0_d(1, (S_d_z0_d){ 2.5, 3.5 }, 4.5, 5);
    C17_CHECK(h4.a == 8.0 && h4.b == 8.5);
    U_z0_f hu;
    hu.a = 2.5f;
    C17_CHECK(clang_u_z0_f(1, hu, 3.5f, 4).a == 11.0f);

    C17_CHECK(clang_bf_f_u32(1, (BF_f_u32){ 2.5f }, 3.5, 4).a == 11.0f);
    C17_CHECK(clang_bf_u32_f(1, (BF_u32_f){ 2.5f }, 3.5, 4).a == 11.0f);
    C17_CHECK(clang_bf_u64_d(2, (BF_u64_d){ 5.5 }, 1.5, 3).d == 12.0);
    C17_CHECK(clang_bf_d_u32(2, (BF_d_u32){ 5.5 }, 1.5, 3).d == 12.0);
    BF_f_u1_f b5 = clang_bf_f_u1_f(1, (BF_f_u1_f){ 2.0f, 3.0f }, 4.5f, 5);
    C17_CHECK(b5.a == 7.5f && b5.b == 8.0f);
    BF_u32_ff b6 = clang_bf_u32_ff(1, (BF_u32_ff){ 2.0f, 3.0f }, 4.5f, 5);
    C17_CHECK(b6.a == 7.5f && b6.b == 8.0f);

    C17_CHECK_EQ(clang_a16_i(10, (A16_i){ 20 }, 30, 4.0).a, 64);
    C17_CHECK(clang_a16_d(10, (A16_d){ 2.5 }, 30, 4.5).a == 47.0);
    C17_CHECK(clang_a16_f(1, (A16_f){ 2.5f }, 3, 4.5).a == 11.0f);
    C17_CHECK_EQ(clang_a16_c(3, (A16_c){ 10 }, 5).c, 18);
    U16_fd u5;
    u5.b = 2.5;
    C17_CHECK(clang_u16_fd(1, u5, 3.5, 4).b == 11.0);
    C17_CHECK_EQ(clang_va_a16_i(1, 2, (A16_i){ 3 }, 4), 4321);
    C17_CHECK(clang_va_a16_d(1, 2, (A16_d){ 3.5 }, 4) == 4371.0);

    /* clang caller -> slimcc-pir callee */
    C17_CHECK_EQ(clang_verify_slimcc_callees(), 0);

    C17_END();
}
