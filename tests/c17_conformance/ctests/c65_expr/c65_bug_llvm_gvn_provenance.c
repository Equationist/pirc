/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.9 / §7.20.1.4 — Real-world compiler bug regressions:
 *   - LLVM PR34548 / PR35229 / PR45725: GVN pointer-equality replacement &
 *     provenance loss when &s.a[1] == &s.b[0], ptrtoint/inttoptr round-trips,
 *     and sequential block-scope lifetime stack-slot reuse.
 */
#include <stdint.h>
#include "c17_test.h"

struct pair_arr {
    int a[1];
    int b[1];
};

/* LLVM PR34548 pattern: if p == q (where p is one-past-end of a, q is &b[0]),
 * GVN must NOT replace *q = val with *p = val and then conclude s->b[0] is
 * unchanged! */
static int test_eq_provenance(struct pair_arr *s, int *p, int *q, int val) {
    s->b[0] = 10;
    if (p == q) {
        *q = val;
    }
    return s->b[0];
}

/* LLVM ptrtoint -> inttoptr round-trip aliasing */
static int test_intptr_roundtrip(int init, int delta) {
    int x = init;
    uintptr_t addr = (uintptr_t)&x;
    int *p = (int *)addr;
    *p += delta;
    return x;
}

/* Conditional pointer selection after equality */
static int test_ptr_select_alias(int *p, int *q) {
    *q = 111;
    int *chosen = (p == q) ? q : p;
    *chosen = 222;
    return *q;
}

/* Sequential block scopes whose stack slots may be reused by the compiler */
static int test_sequential_scopes(int seed) {
    int sum = 0;
    uintptr_t addr1 = 0;
    {
        int local1[4];
        local1[0] = seed;
        local1[1] = seed + 1;
        local1[2] = seed + 2;
        local1[3] = seed + 3;
        addr1 = (uintptr_t)&local1[0];
        sum += local1[0] + local1[3];
    }
    {
        int local2[4];
        local2[0] = seed * 10;
        local2[1] = seed * 10 + 1;
        local2[2] = seed * 10 + 2;
        local2[3] = seed * 10 + 3;
        uintptr_t addr2 = (uintptr_t)&local2[0];
        /* Even if addr1 == addr2 (stack reuse), local2 must hold its own values */
        if (addr1 == addr2) {
            local2[0] += 5;
            sum += local2[0] - 5;
        } else {
            sum += local2[0];
        }
        sum += local2[3];
    }
    return sum;
}

int main(void) {
    C17_BEGIN("c65_bug_llvm_gvn_provenance",
              "LLVM PR34548/PR35229/PR45725 GVN pointer provenance & lifetime");

    /* Adjacent array members inside a struct: &s.a[1] == &s.b[0] is guaranteed
     * because int has no padding between two int[1] members. */
    {
        struct pair_arr s;
        s.a[0] = 1;
        s.b[0] = 2;
        int *p = &s.a[1];
        int *q = &s.b[0];
        C17_CHECK(p == q);
        int got = test_eq_provenance(&s, p, q, c17_opaque_int(42));
        C17_CHECK_EQ(got, 42);
        C17_CHECK_EQ(s.b[0], 42);
    }

    /* uintptr_t round-trip preserves aliasing */
    C17_CHECK_EQ(test_intptr_roundtrip(c17_opaque_int(30), c17_opaque_int(12)), 42);

    /* Conditional pointer select with equal & unequal pointers */
    {
        int x = 5;
        int y = 6;
        C17_CHECK_EQ(test_ptr_select_alias(&x, &x), 222);
        C17_CHECK_EQ(x, 222);
        C17_CHECK_EQ(test_ptr_select_alias(&x, &y), 111);
        C17_CHECK_EQ(x, 222);
        C17_CHECK_EQ(y, 111);
    }

    /* Sequential block scopes with potential stack-slot reuse */
    {
        int s = c17_opaque_int(3);
        /* (3 + 6) + (30 + 33) = 72 */
        C17_CHECK_EQ(test_sequential_scopes(s), 72);
    }

    C17_END();
}
