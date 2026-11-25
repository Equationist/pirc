/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §6.7.3.1 / §7.13 — Real-world compiler bug regressions:
 *   - gcc.c-torture/execute DSE across escaped pointers & function calls
 *   - Partial overwrite of by-value returned struct after inlining
 *   - GCC PR49279: struct with restrict pointer member
 *   - setjmp/longjmp store survival (DSE must not drop visible stores)
 *   - Struct self-assignment (s = s)
 */
#include <setjmp.h>
#include <stdint.h>
#include "c17_test.h"

struct triple {
    int a;
    int b;
    int c;
};

static struct triple make_triple(int x) {
    struct triple t;
    t.a = x;
    t.b = x * 2;
    t.c = x * 3;
    return t;
}

static int read_via_ptr(const int *p) {
    volatile const int *vp = p;
    return *vp;
}

static int dse_escaped_store(int x) {
    int v = x;
    int r1 = read_via_ptr(&v);
    v = x + 100;
    int r2 = read_via_ptr(&v);
    v = x + 200;
    return r1 + r2 + v;
}

/* GCC PR49279: restrict pointer inside a struct */
struct res_box {
    int tag;
    int *restrict p;
};

static int sum_restrict_boxes(struct res_box a, struct res_box b) {
    *a.p = a.tag * 10;
    *b.p = b.tag * 20;
    return *a.p + *b.p;
}

static jmp_buf g_jmp;
static int g_jmp_side = 0;

static void do_longjmp(int *out, int val) {
    *out = val;
    longjmp(g_jmp, 1);
}

int main(void) {
    C17_BEGIN("c65_bug_gcc_torture_dse_aliasing",
              "gcc.c-torture DSE, struct return overwrite, PR49279 restrict, setjmp");

    /* DSE must preserve every store visible through escaped pointer */
    {
        int x = c17_opaque_int(7);
        /* 7 + 107 + 207 = 321 */
        C17_CHECK_EQ(dse_escaped_store(x), 321);
    }

    /* Partial overwrite of returned struct */
    {
        struct triple t = make_triple(c17_opaque_int(11));
        t.b = 999;
        C17_CHECK_EQ(t.a, 11);
        C17_CHECK_EQ(t.b, 999);
        C17_CHECK_EQ(t.c, 33);

        /* Self-assignment must leave struct intact */
        t = t;
        C17_CHECK_EQ(t.a, 11);
        C17_CHECK_EQ(t.b, 999);
        C17_CHECK_EQ(t.c, 33);
    }

    /* GCC PR49279 restrict member */
    {
        int x = 0;
        int y = 0;
        struct res_box b1 = { c17_opaque_int(3), &x };
        struct res_box b2 = { c17_opaque_int(4), &y };
        C17_CHECK_EQ(sum_restrict_boxes(b1, b2), 110);
        C17_CHECK_EQ(x, 30);
        C17_CHECK_EQ(y, 80);
    }

    /* setjmp / longjmp (C17 §7.13.2.1 p3):
     * - volatile auto local modified between setjmp and longjmp MUST survive
     * - non-volatile auto local NOT modified after setjmp MUST survive
     * - static-duration object modified before longjmp MUST survive */
    {
        volatile int stage = 0;
        int pre_local = c17_opaque_int(123);
        g_jmp_side = 0;
        if (setjmp(g_jmp) == 0) {
            stage = 1;
            do_longjmp(&g_jmp_side, c17_opaque_int(777));
            stage = 2; /* unreachable */
        }
        C17_CHECK_EQ(stage, 1);
        C17_CHECK_EQ(pre_local, 123);
        C17_CHECK_EQ(g_jmp_side, 777);
    }

    C17_END();
}
