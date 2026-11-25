/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8 — Adversarial simplify_cfg.c stress:
 *  - Loop rotation (try_rotate_one_loop) where header phi/IV values are used
 *    after loop exit (0-trip, 1-trip, negative-bound, multi-trip)
 *  - Tail-duplication of tiny latch blocks after switch inside for-loop
 *  - Accumulator TCO (ADD & MUL) + double recursion + mixed base cases
 *  - Chained struct assignment (a = b = c) and struct rvalue member access
 */
#include <stdint.h>
#include "c17_test.h"

/* 1. Top-tested loop whose IV & accumulator are BOTH used after exit (0..N trips) */
static int64_t rotated_loop_ext_use(const int *a, int n, int *out_final_i) {
    int i = 0;
    int64_t acc = 17;
    while (i < n) {
        acc += (int64_t)a[i] * (int64_t)(i + 1);
        i++;
    }
    *out_final_i = i;
    return acc + (int64_t)i * 100;
}

/* 2. Switch inside for-loop jumping to shared 1-inst `i++` latch (tail_dup_tiny_jmp_blocks) */
static int switch_shared_latch(const int *ops, int n) {
    int acc = 0;
    for (int i = 0; i < n; i++) {
        switch (ops[i]) {
        case 0: acc += 1; break;
        case 1: acc += 10; break;
        case 2: acc -= 3; break;
        case 3: acc ^= 0x55; break;
        default: acc += 7; break;
        }
    }
    return acc;
}

/* 3. MUL-accumulator TCO & double-recursion ADD-TCO */
static int64_t fact_mul_tco(int64_t n) {
    if (n <= 1) return 1;
    return n * fact_mul_tco(n - 1);
}

static int fib_double_rec(int n) {
    if (n <= 1) return n;
    return fib_double_rec(n - 1) + fib_double_rec(n - 2);
}

/* 4. Chained struct assignment & rvalue member access (TCC/chibicc/kefir classic bug) */
struct nested_s {
    int id;
    union {
        struct { int x; int y; } pt;
        uint64_t raw;
    } u;
};

static struct nested_s make_nested(int id, int x, int y) {
    struct nested_s s;
    s.id = id;
    s.u.pt.x = x;
    s.u.pt.y = y;
    return s;
}

static int64_t (*volatile g_rot_fn)(const int *, int, int *) = rotated_loop_ext_use;
static int (*volatile g_sw_latch)(const int *, int) = switch_shared_latch;
static int64_t (*volatile g_fact)(int64_t) = fact_mul_tco;
static int (*volatile g_fib)(int) = fib_double_rec;
static struct nested_s (*volatile g_make_ns)(int, int, int) = make_nested;

int main(void) {
    C17_BEGIN("c68_bug_cfg_rotate_taildup_tco_struct_chain",
              "Loop rotation ext-use, switch tail-dup, MUL/fib TCO, chained struct assign");

    /* 1. Rotated loop external IV/acc uses across 0, negative, 1, and 5 trips */
    {
        int arr[5] = { 2, 4, 6, 8, 10 };
        int fi = -1;
        C17_CHECK_EQ(g_rot_fn(arr, c17_opaque_int(0), &fi), 17);
        C17_CHECK_EQ(fi, 0);
        C17_CHECK_EQ(g_rot_fn(arr, c17_opaque_int(-3), &fi), 17);
        C17_CHECK_EQ(fi, 0);
        /* n=1: acc=17+2=19, i=1 => 119 */
        C17_CHECK_EQ(g_rot_fn(arr, c17_opaque_int(1), &fi), 119);
        C17_CHECK_EQ(fi, 1);
        /* n=5: acc=17 + 2*1 + 4*2 + 6*3 + 8*4 + 10*5 = 17+2+8+18+32+50 = 127; +500 = 627 */
        C17_CHECK_EQ(g_rot_fn(arr, c17_opaque_int(5), &fi), 627);
        C17_CHECK_EQ(fi, 5);
    }

    /* 2. Switch with shared latch */
    {
        int ops[8] = { 0, 1, 2, 3, 9, 1, 0, 2 };
        /* 0->1, 1->11, 2->8, 3->8^85=93, 9->100, 1->110, 0->111, 2->108 */
        C17_CHECK_EQ(g_sw_latch(ops, c17_opaque_int(8)), 108);
        C17_CHECK_EQ(g_sw_latch(ops, c17_opaque_int(0)), 0);
    }

    /* 3. MUL-accumulator TCO & double-recursion fib */
    C17_CHECK_EQ(g_fact(c17_opaque_int(0)), 1);
    C17_CHECK_EQ(g_fact(c17_opaque_int(1)), 1);
    C17_CHECK_EQ(g_fact(c17_opaque_int(12)), INT64_C(479001600));
    C17_CHECK_EQ(g_fact(c17_opaque_int(20)), INT64_C(2432902008176640000));
    C17_CHECK_EQ(g_fib(c17_opaque_int(0)), 0);
    C17_CHECK_EQ(g_fib(c17_opaque_int(1)), 1);
    C17_CHECK_EQ(g_fib(c17_opaque_int(10)), 55);
    C17_CHECK_EQ(g_fib(c17_opaque_int(15)), 610);

    /* 4. Chained struct assignment & rvalue member access */
    {
        struct nested_s a = { 0, { { 0, 0 } } };
        struct nested_s b = { 0, { { 0, 0 } } };
        struct nested_s c = g_make_ns(c17_opaque_int(7), c17_opaque_int(11), c17_opaque_int(22));

        int got_y = (a = b = c).u.pt.y;
        C17_CHECK_EQ(got_y, 22);
        C17_CHECK_EQ(a.id, 7);
        C17_CHECK_EQ(a.u.pt.x, 11);
        C17_CHECK_EQ(b.id, 7);
        C17_CHECK_EQ(b.u.pt.y, 22);

        /* Direct rvalue member access from function call & ternary */
        C17_CHECK_EQ(g_make_ns(3, 40, 50).u.pt.x, 40);
        int cond = c17_opaque_int(0);
        C17_CHECK_EQ((cond ? g_make_ns(1, 2, 3) : g_make_ns(4, 5, 6)).u.pt.y, 6);
    }

    C17_END();
}
