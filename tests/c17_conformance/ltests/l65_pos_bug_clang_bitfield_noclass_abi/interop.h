/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Shared declarations for bidirectional slimcc-pir <-> clang ABI interop:
 *   1. AAPCS64 HFAs containing zero-width unnamed bitfields (`int : 0`).
 *   2. SysV AMD64 unnamed bitfields as NO_CLASS padding (mixed & all-padding eightbytes).
 *   3. SysV AMD64 _Alignas(16) NO_CLASS trailing eightbytes (fixed & variadic).
 */
#ifndef INTEROP_BITFIELD_NOCLASS_H
#define INTEROP_BITFIELD_NOCLASS_H

/* AAPCS64 HFA with zero-width bitfields */
typedef struct { float a; int : 0; float b; } S_f_z0_f;
typedef struct { int : 0; float a; float b; } S_z0_ff;
typedef struct { float a; float b; int : 0; } S_ff_z0;
typedef struct { double a; int : 0; double b; } S_d_z0_d;
typedef union  { int : 0; float a; } U_z0_f;

/* SysV AMD64 unnamed bitfields (NO_CLASS padding) */
typedef struct { float a; int : 32; } BF_f_u32;
typedef struct { int : 32; float a; } BF_u32_f;
typedef struct { int : 32; int : 32; double d; } BF_u64_d;
typedef struct { double d; int : 32; } BF_d_u32;
typedef struct { float a; unsigned : 1; float b; } BF_f_u1_f;
typedef struct { int : 32; float a; float b; } BF_u32_ff;

/* SysV AMD64 _Alignas(16) trailing NO_CLASS eightbytes */
typedef struct { _Alignas(16) int a; } A16_i;
typedef struct { _Alignas(16) double a; } A16_d;
typedef struct { _Alignas(16) float a; } A16_f;
typedef struct { _Alignas(16) char c; } A16_c;
typedef union  { _Alignas(16) float a; double b; } U16_fd;

/* Clang-compiled callees (called from slimcc-pir) */
S_f_z0_f  clang_f_z0_f(int k, S_f_z0_f v, float f, int t);
S_z0_ff   clang_z0_ff(int k, S_z0_ff v, float f, int t);
S_ff_z0   clang_ff_z0(int k, S_ff_z0 v, float f, int t);
S_d_z0_d  clang_d_z0_d(int k, S_d_z0_d v, double d, int t);
U_z0_f    clang_u_z0_f(int k, U_z0_f v, float f, int t);

BF_f_u32  clang_bf_f_u32(int k, BF_f_u32 v, double d, int t);
BF_u32_f  clang_bf_u32_f(int k, BF_u32_f v, double d, int t);
BF_u64_d  clang_bf_u64_d(int k, BF_u64_d v, double d, int t);
BF_d_u32  clang_bf_d_u32(int k, BF_d_u32 v, double d, int t);
BF_f_u1_f clang_bf_f_u1_f(int k, BF_f_u1_f v, float f, int t);
BF_u32_ff clang_bf_u32_ff(int k, BF_u32_ff v, float f, int t);

A16_i     clang_a16_i(int x, A16_i s, int y, double z);
A16_d     clang_a16_d(int x, A16_d s, int y, double z);
A16_f     clang_a16_f(int x, A16_f s, int y, double z);
A16_c     clang_a16_c(int x, A16_c s, int y);
U16_fd    clang_u16_fd(int x, U16_fd u, double d, int y);
int       clang_va_a16_i(int tag, ...);
double    clang_va_a16_d(int tag, ...);

/* slimcc-pir-compiled callees (called from clang) */
S_f_z0_f  slimcc_f_z0_f(int k, S_f_z0_f v, float f, int t);
S_z0_ff   slimcc_z0_ff(int k, S_z0_ff v, float f, int t);
S_ff_z0   slimcc_ff_z0(int k, S_ff_z0 v, float f, int t);
S_d_z0_d  slimcc_d_z0_d(int k, S_d_z0_d v, double d, int t);
U_z0_f    slimcc_u_z0_f(int k, U_z0_f v, float f, int t);

BF_f_u32  slimcc_bf_f_u32(int k, BF_f_u32 v, double d, int t);
BF_u32_f  slimcc_bf_u32_f(int k, BF_u32_f v, double d, int t);
BF_u64_d  slimcc_bf_u64_d(int k, BF_u64_d v, double d, int t);
BF_d_u32  slimcc_bf_d_u32(int k, BF_d_u32 v, double d, int t);
BF_f_u1_f slimcc_bf_f_u1_f(int k, BF_f_u1_f v, float f, int t);
BF_u32_ff slimcc_bf_u32_ff(int k, BF_u32_ff v, float f, int t);

A16_i     slimcc_a16_i(int x, A16_i s, int y, double z);
A16_d     slimcc_a16_d(int x, A16_d s, int y, double z);
A16_f     slimcc_a16_f(int x, A16_f s, int y, double z);
A16_c     slimcc_a16_c(int x, A16_c s, int y);
U16_fd    slimcc_u16_fd(int x, U16_fd u, double d, int y);
int       slimcc_va_a16_i(int tag, ...);
double    slimcc_va_a16_d(int tag, ...);

int clang_verify_slimcc_callees(void);

#endif
