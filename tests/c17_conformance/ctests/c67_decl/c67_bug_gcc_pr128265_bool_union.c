/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.3.1.2 / §6.7.2.1 — Real-world compiler bug regressions:
 *   - GCC PR128265: conditional read of _Bool union member must not speculatively
 *     load inactive union bytes (non-0/1 bit patterns)
 *   - _Bool conversion truncation bugs (e.g. (_Bool)256, (_Bool)(1ULL << 40),
 *     (_Bool)(-0.0), (_Bool)NAN, (_Bool)0.0001)
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include "c17_test.h"

struct tagged_val {
    int type; /* 0 = ll, 1 = bool */
    union {
        _Bool b;
        long long ll;
    } u;
};

static int eval_tagged(struct tagged_val v) {
    /* If optimizer speculatively loads v.u.b before checking v.type == 1,
     * v.u.b holds garbage bytes from v.u.ll (e.g. 0x02 or 0xFF) which breaks
     * _Bool 0/1 range assumptions. */
    if (v.type == 1 && v.u.b) {
        return 100;
    }
    if (v.type == 1 && !v.u.b) {
        return 200;
    }
    return (int)(v.u.ll & 0xFF);
}

static int eval_tagged_ternary(const struct tagged_val *p) {
    int r = (p->type == 1) ? (p->u.b ? 7 : 3) : -1;
    return r;
}

static _Bool to_bool_int(int x) { return (_Bool)x; }
static _Bool to_bool_ull(unsigned long long x) { return (_Bool)x; }
static _Bool to_bool_dbl(double x) { return (_Bool)x; }
static _Bool to_bool_flt(float x) { return (_Bool)x; }

int main(void) {
    C17_BEGIN("c67_bug_gcc_pr128265_bool_union",
              "GCC PR128265 _Bool tagged union & _Bool conversion edge cases");

    /* Inactive _Bool arm holds non-0/1 payload bytes (0x02, 0xFE, 0x00000100) */
    {
        struct tagged_val a;
        a.type = c17_opaque_int(0);
        a.u.ll = c17_opaque_ll(0x123456789ABCDEF2LL);
        C17_CHECK_EQ(eval_tagged(a), 0xF2);
        C17_CHECK_EQ(eval_tagged_ternary(&a), -1);

        struct tagged_val b;
        b.type = c17_opaque_int(0);
        b.u.ll = c17_opaque_ll(0x0000000000000100LL);
        C17_CHECK_EQ(eval_tagged(b), 0x00);
        C17_CHECK_EQ(eval_tagged_ternary(&b), -1);

        struct tagged_val c;
        c.type = c17_opaque_int(1);
        c.u.b = (c17_opaque_int(42) != 0);
        C17_CHECK_EQ(eval_tagged(c), 100);
        C17_CHECK_EQ(eval_tagged_ternary(&c), 7);

        struct tagged_val d;
        d.type = c17_opaque_int(1);
        d.u.b = false;
        C17_CHECK_EQ(eval_tagged(d), 200);
        C17_CHECK_EQ(eval_tagged_ternary(&d), 3);
    }

    /* _Bool conversions where low byte is 0x00 (classic truncation bug!) */
    C17_CHECK_EQ((int)to_bool_int(c17_opaque_int(0)), 0);
    C17_CHECK_EQ((int)to_bool_int(c17_opaque_int(256)), 1);
    C17_CHECK_EQ((int)to_bool_int(c17_opaque_int(65536)), 1);
    C17_CHECK_EQ((int)to_bool_int(c17_opaque_int(-256)), 1);
    C17_CHECK_EQ((int)to_bool_int(c17_opaque_int(0x7FFFFF00)), 1);

    C17_CHECK_EQ((int)to_bool_ull(c17_opaque_ull(0ULL)), 0);
    C17_CHECK_EQ((int)to_bool_ull(c17_opaque_ull(1ULL << 32)), 1);
    C17_CHECK_EQ((int)to_bool_ull(c17_opaque_ull(1ULL << 63)), 1);

    /* Floating-point to _Bool conversions */
    C17_CHECK_EQ((int)to_bool_dbl(c17_opaque_dbl(0.0)), 0);
    C17_CHECK_EQ((int)to_bool_dbl(c17_opaque_dbl(-0.0)), 0);
    C17_CHECK_EQ((int)to_bool_dbl(c17_opaque_dbl(1e-300)), 1);
    C17_CHECK_EQ((int)to_bool_dbl(c17_opaque_dbl(-0.5)), 1);
    C17_CHECK_EQ((int)to_bool_dbl(c17_opaque_dbl((double)NAN)), 1);
    C17_CHECK_EQ((int)to_bool_flt(c17_opaque_flt(-0.0f)), 0);
    C17_CHECK_EQ((int)to_bool_flt(c17_opaque_flt(1e-30f)), 1);

    /* Arithmetic on _Bool values must always treat true as 1 */
    {
        _Bool b1 = (_Bool)c17_opaque_int(256);
        _Bool b2 = (_Bool)c17_opaque_int(512);
        C17_CHECK_EQ((int)b1 + (int)b2, 2);
        b1 += 5;
        C17_CHECK_EQ((int)b1, 1);
        b1++;
        C17_CHECK_EQ((int)b1, 1);
        b1--;
        C17_CHECK_EQ((int)b1, 0);
        b1--;
        C17_CHECK_EQ((int)b1, 1);
    }

    C17_END();
}
