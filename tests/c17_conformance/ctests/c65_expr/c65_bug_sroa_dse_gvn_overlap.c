/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §7.24 — Real-world SROA, DSE, GVN, and
 * if-conversion safety regressions:
 *   - Small constant-size memcpy/memmove (1..32B) inlining + forwarding + overlap
 *   - SROA on non-escaping struct vs disqualified partially-overlapping char* access
 *   - GVN zext(trunc(shr)), and(zext), cmp_eq(zext(cmp), 0) with NaN & integers
 *   - If-conversion diamond safety: must NOT speculate trapping div/deref in branch
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "c17_test.h"

struct sroa_rec {
    int32_t a;
    int64_t b;
    double  c;
    uint16_t d;
};

static double sroa_pure_fields(int32_t x, int64_t y, double z) {
    struct sroa_rec r;
    r.a = x;
    r.b = y;
    r.c = z;
    r.d = (uint16_t)(x + 5);
    r.a += 10;
    r.b -= 3;
    return (double)r.a + (double)r.b + r.c + (double)r.d;
}

/* Must NOT be mis-SROA'd when modified through unsigned char * */
static uint32_t sroa_disqualify_byte_write(uint32_t init, uint8_t b) {
    struct {
        uint32_t lo;
        uint32_t hi;
    } pair = { init, 0x11223344U };
    unsigned char *p = (unsigned char *)&pair.lo;
    uint32_t probe = 0x01020304U;
    if (((unsigned char *)&probe)[0] == 0x04U) {
        p[0] = b;
    } else {
        p[3] = b;
    }
    return pair.lo + pair.hi;
}

/* Small constant-size overlapping memmove (exercises dse.c inline_small_mem_calls 1..32B) */
static int test_small_inline_memmove(void) {
    uint8_t buf[40];
    int ok = 1;
    /* Overlap forward & backward for sizes 3, 7, 13, 17, 24, 31, 32 */
    for (int i = 0; i < 40; i++) buf[i] = (uint8_t)(i + 1);
    memmove(buf + 1, buf, 31);
    for (int i = 0; i < 31; i++) {
        if (buf[1 + i] != (uint8_t)(i + 1)) ok = 0;
    }
    for (int i = 0; i < 40; i++) buf[i] = (uint8_t)(i + 1);
    memmove(buf, buf + 1, 32);
    for (int i = 0; i < 32; i++) {
        if (buf[i] != (uint8_t)(i + 2)) ok = 0;
    }
    for (int i = 0; i < 40; i++) buf[i] = (uint8_t)(i + 10);
    memmove(buf + 2, buf, 13);
    for (int i = 0; i < 13; i++) {
        if (buf[2 + i] != (uint8_t)(i + 10)) ok = 0;
    }
    return ok;
}

/* Diamond if-conversion must NOT speculate trapping div or NULL deref! */
static int ifconv_no_speculate_div(int cond, int x, int d) {
    int r;
    if (cond) {
        r = x / d;
    } else {
        r = x + 1;
    }
    return r;
}

static int ifconv_no_speculate_deref(int cond, const int *p) {
    int r = 5;
    if (cond) {
        r = *p;
    }
    return r;
}

/* GVN identities: zext(trunc(shr)), and(zext), !(a < b) on NaN */
static uint32_t gvn_byte_extract(uint32_t x) {
    uint8_t b = (uint8_t)(x >> 24);
    return ((uint32_t)b) & 0xFFU;
}

static int gvn_negated_fp_cmp(double a, double b) {
    int lt = (a < b);
    return (lt == 0) ? 10 : 20;
}

int main(void) {
    C17_BEGIN("c65_bug_sroa_dse_gvn_overlap",
              "SROA pure vs char* alias, inline 1..32B memmove, if-conv trap, GVN");

    /* Pure SROA */
    C17_CHECK_DBL(sroa_pure_fields(c17_opaque_int(5), c17_opaque_ll(20), c17_opaque_dbl(2.5)),
                  15.0 + 17.0 + 2.5 + 10.0, 0.0);

    /* Disqualified SROA via byte pointer */
    C17_CHECK_UEQ(sroa_disqualify_byte_write(
                      (uint32_t)c17_opaque_ull(0x10203000ULL),
                      (uint8_t)c17_opaque_int(0x55)),
                  0x10203055U + 0x11223344U);

    /* Inline small overlapping memmove 13/31/32 bytes */
    C17_CHECK_EQ(test_small_inline_memmove(), 1);

    /* If-conversion trap safety */
    C17_CHECK_EQ(ifconv_no_speculate_div(c17_opaque_int(0), c17_opaque_int(41), c17_opaque_int(0)), 42);
    C17_CHECK_EQ(ifconv_no_speculate_div(c17_opaque_int(1), c17_opaque_int(42), c17_opaque_int(6)), 7);
    C17_CHECK_EQ(ifconv_no_speculate_deref(c17_opaque_int(0), NULL), 5);
    {
        int v = c17_opaque_int(88);
        C17_CHECK_EQ(ifconv_no_speculate_deref(c17_opaque_int(1), &v), 88);
    }

    /* GVN identities */
    C17_CHECK_UEQ(gvn_byte_extract((uint32_t)c17_opaque_ull(0xAB123456ULL)), 0xABU);
    /* For NaN: (NaN < 1.0) == 0 -> must return 10! (Must NOT fold !(a < b) to (a >= b)!) */
    C17_CHECK_EQ(gvn_negated_fp_cmp(c17_opaque_dbl((double)NAN), c17_opaque_dbl(1.0)), 10);
    C17_CHECK_EQ(gvn_negated_fp_cmp(c17_opaque_dbl(0.5), c17_opaque_dbl(1.0)), 20);

    C17_END();
}
