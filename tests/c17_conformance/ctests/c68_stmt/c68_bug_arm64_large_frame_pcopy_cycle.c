/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.5 / §6.8.6.1 — Regression test for:
 *   1. ARM64 large-frame parallel-copy cycle scratch-register clobber
 *      (regalloc lower_pcopy_bundle cyc_gp colliding with arm64_frame
 *      MI_STORE_SLOT / FP MI_LOAD_SLOT address scratch when fp_offset < -256).
 *   2. mem2reg transitive zero-offset ptr_add chain across out-of-order
 *      goto blocks where a dominating definition appears after its user.
 */
#include <stdint.h>
#include "c17_test.h"

static uint64_t cycle_large_frame(uint64_t n, uint64_t seed) {
    volatile uint64_t pad[64];
    for (int i = 0; i < 64; i++) pad[i] = seed + (uint64_t)i;

    uint64_t v0  = seed + 1,  v1  = seed + 2,  v2  = seed + 3,  v3  = seed + 4;
    uint64_t v4  = seed + 5,  v5  = seed + 6,  v6  = seed + 7,  v7  = seed + 8;
    uint64_t v8  = seed + 9,  v9  = seed + 10, v10 = seed + 11, v11 = seed + 12;
    uint64_t v12 = seed + 13, v13 = seed + 14, v14 = seed + 15, v15 = seed + 16;
    uint64_t v16 = seed + 17, v17 = seed + 18, v18 = seed + 19, v19 = seed + 20;
    uint64_t v20 = seed + 21, v21 = seed + 22, v22 = seed + 23, v23 = seed + 24;
    uint64_t v24 = seed + 25, v25 = seed + 26;

    double f0 = 1.0,  f1 = 2.0,  f2 = 3.0,  f3 = 4.0,  f4 = 5.0,  f5 = 6.0;
    double f6 = 7.0,  f7 = 8.0,  f8 = 9.0,  f9 = 10.0, f10 = 11.0, f11 = 12.0;
    double f12 = 13.0, f13 = 14.0, f14 = 15.0, f15 = 16.0, f16 = 17.0, f17 = 18.0;

    for (uint64_t k = 0; k < n; k++) {
        uint64_t t = v0;
        v0 = v1; v1 = v2; v2 = v3; v3 = v4; v4 = v5; v5 = v6; v6 = v7; v7 = v8;
        v8 = v9; v9 = v10; v10 = v11; v11 = v12; v12 = v13; v13 = v14; v14 = v15; v15 = v16;
        v16 = v17; v17 = v18; v18 = v19; v19 = v20; v20 = v21; v21 = v22; v22 = v23; v23 = v24;
        v24 = v25; v25 = t;

        double ft = f0;
        f0 = f1; f1 = f2; f2 = f3; f3 = f4; f4 = f5; f5 = f6; f6 = f7; f7 = f8;
        f8 = f9; f9 = f10; f10 = f11; f11 = f12; f12 = f13; f13 = f14; f14 = f15; f15 = f16;
        f16 = f17; f17 = ft;
    }

    uint64_t acc = pad[0] + pad[63];
    acc += v0*1 + v1*2 + v2*3 + v3*4 + v4*5 + v5*6 + v6*7 + v7*8 + v8*9 + v9*10;
    acc += v10*11 + v11*12 + v12*13 + v13*14 + v14*15 + v15*16 + v16*17 + v17*18;
    acc += v18*19 + v19*20 + v20*21 + v21*22 + v22*23 + v23*24 + v24*25 + v25*26;
    acc += (uint64_t)(f0*1 + f1*2 + f2*3 + f3*4 + f4*5 + f5*6 + f6*7 + f7*8 + f8*9 +
                      f9*10 + f10*11 + f11*12 + f12*13 + f13*14 + f14*15 + f15*16 + f16*17 + f17*18);
    return acc;
}

static uint64_t cycle_48_spills(uint64_t n, uint64_t s) {
    uint64_t a0=s+0, a1=s+1, a2=s+2, a3=s+3, a4=s+4, a5=s+5, a6=s+6, a7=s+7;
    uint64_t a8=s+8, a9=s+9, a10=s+10, a11=s+11, a12=s+12, a13=s+13, a14=s+14, a15=s+15;
    uint64_t a16=s+16, a17=s+17, a18=s+18, a19=s+19, a20=s+20, a21=s+21, a22=s+22, a23=s+23;
    uint64_t a24=s+24, a25=s+25, a26=s+26, a27=s+27, a28=s+28, a29=s+29, a30=s+30, a31=s+31;
    uint64_t a32=s+32, a33=s+33, a34=s+34, a35=s+35, a36=s+36, a37=s+37, a38=s+38, a39=s+39;
    uint64_t a40=s+40, a41=s+41, a42=s+42, a43=s+43, a44=s+44, a45=s+45, a46=s+46, a47=s+47;

    for (uint64_t k = 0; k < n; k++) {
        uint64_t t = a0;
        a0=a1; a1=a2; a2=a3; a3=a4; a4=a5; a5=a6; a6=a7; a7=a8;
        a8=a9; a9=a10; a10=a11; a11=a12; a12=a13; a13=a14; a14=a15; a15=a16;
        a16=a17; a17=a18; a18=a19; a19=a20; a20=a21; a21=a22; a22=a23; a23=a24;
        a24=a25; a25=a26; a26=a27; a27=a28; a28=a29; a29=a30; a30=a31; a31=a32;
        a32=a33; a33=a34; a34=a35; a35=a36; a36=a37; a37=a38; a38=a39; a39=a40;
        a40=a41; a41=a42; a42=a43; a43=a44; a44=a45; a45=a46; a46=a47; a47=t;
    }

    uint64_t arr[48] = {
        a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11,a12,a13,a14,a15,
        a16,a17,a18,a19,a20,a21,a22,a23,a24,a25,a26,a27,a28,a29,a30,a31,
        a32,a33,a34,a35,a36,a37,a38,a39,a40,a41,a42,a43,a44,a45,a46,a47
    };
    uint64_t h = 0;
    for (int i = 0; i < 48; i++) h = h * 31u + arr[i];
    return h;
}

struct Box { int x; int y; };

static int out_of_order_zero_alias(int c) {
    struct Box b = { 10, 20 };
    int *p1 = NULL;
    int *p2 = NULL;
    if (c == 0) goto make_p1;
    goto skip_init;

use_p2:
    p2 = p1 + 0;
    return *p2 + b.y;

make_p1:
    p1 = &b.x + 0;
    goto use_p2;

skip_init:
    return b.y;
}

int main(void) {
    C17_BEGIN("c68_bug_arm64_large_frame_pcopy_cycle",
              "ARM64 large-frame parallel-copy cycle scratch isolation & mem2reg zero_alias");
    uint64_t (*volatile fn_lf)(uint64_t, uint64_t) = cycle_large_frame;
    uint64_t (*volatile fn_48)(uint64_t, uint64_t) = cycle_48_spills;
    int (*volatile fn_goto)(int) = out_of_order_zero_alias;

    /* Compute expected reference values directly via arrays */
    uint64_t v[26];
    for (int i = 0; i < 26; i++) v[i] = 100u + (uint64_t)(i + 1);
    double f[18];
    for (int i = 0; i < 18; i++) f[i] = (double)(i + 1);
    for (int k = 0; k < 7; k++) {
        uint64_t t = v[0];
        for (int i = 0; i < 25; i++) v[i] = v[i + 1];
        v[25] = t;
        double ft = f[0];
        for (int i = 0; i < 17; i++) f[i] = f[i + 1];
        f[17] = ft;
    }
    uint64_t exp1 = 100u + 163u;
    for (int i = 0; i < 26; i++) exp1 += v[i] * (uint64_t)(i + 1);
    double fsum = 0.0;
    for (int i = 0; i < 18; i++) fsum += f[i] * (double)(i + 1);
    exp1 += (uint64_t)fsum;

    uint64_t a[48];
    for (int i = 0; i < 48; i++) a[i] = 1000u + (uint64_t)i;
    for (int k = 0; k < 13; k++) {
        uint64_t t = a[0];
        for (int i = 0; i < 47; i++) a[i] = a[i + 1];
        a[47] = t;
    }
    uint64_t exp2 = 0;
    for (int i = 0; i < 48; i++) exp2 = exp2 * 31u + a[i];

    C17_CHECK_UEQ(fn_lf(7, 100), exp1);
    C17_CHECK_UEQ(fn_48(13, 1000), exp2);
    C17_CHECK_EQ(fn_goto(0), 30);
    C17_CHECK_EQ(fn_goto(1), 20);
    C17_END();
}
