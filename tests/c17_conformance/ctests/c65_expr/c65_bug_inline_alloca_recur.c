/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 / §6.7.4 — Real-world inliner & recursion bugs:
 *   - Inlining callee with local struct/array inside a loop (no alloca leak/aliasing)
 *   - Inlining callee with multiple return paths & static local variable
 *   - Mutual recursion & self-recursion with struct pass/return
 *   - Direct call + indirect function pointer call to the same small helper
 */
#include "c17_test.h"

struct pair {
    int a;
    int b;
};

static int multi_ret_static(int x) {
    static int calls = 0;
    calls++;
    if (x < 0) {
        return -calls;
    }
    if (x == 0) {
        return calls * 10;
    }
    return calls * 100 + x;
}

static struct pair local_buf_helper(int base) {
    int tmp[8];
    for (int i = 0; i < 8; i++) {
        tmp[i] = base + i;
    }
    struct pair p;
    p.a = tmp[0] + tmp[7];
    p.b = tmp[3] * tmp[4];
    return p;
}

static int is_odd(int n);

static int is_even(int n) {
    if (n == 0) return 1;
    return is_odd(n - 1);
}

static int is_odd(int n) {
    if (n == 0) return 0;
    return is_even(n - 1);
}

static struct pair fib_pair(int n) {
    if (n <= 0) {
        struct pair z = { 0, 1 };
        return z;
    }
    struct pair prev = fib_pair(n - 1);
    struct pair cur = { prev.b, prev.a + prev.b };
    return cur;
}

int main(void) {
    C17_BEGIN("c65_bug_inline_alloca_recur",
              "Inliner static local, loop allocas, multi-return, mutual & struct recursion");

    /* Static local inside inlined multi-return helper must share one counter
     * across both direct calls and indirect function-pointer calls */
    {
        int (*fp)(int) = multi_ret_static;
        C17_CHECK_EQ(multi_ret_static(c17_opaque_int(5)), 105);  /* calls=1 */
        C17_CHECK_EQ(multi_ret_static(c17_opaque_int(0)), 20);   /* calls=2 */
        C17_CHECK_EQ(fp(c17_opaque_int(-1)), -3);                /* calls=3 */
        C17_CHECK_EQ(multi_ret_static(c17_opaque_int(7)), 407);  /* calls=4 */
    }

    /* Callee with local array + struct return called repeatedly in a loop */
    {
        int sum_a = 0;
        int sum_b = 0;
        for (int i = 1; i <= 50; i++) {
            struct pair r = local_buf_helper(c17_opaque_int(i));
            sum_a += r.a;
            sum_b += r.b;
        }
        /* For each i: a = 2*i + 7; b = (i+3)*(i+4) */
        int want_a = 0;
        int want_b = 0;
        for (int i = 1; i <= 50; i++) {
            want_a += 2 * i + 7;
            want_b += (i + 3) * (i + 4);
        }
        C17_CHECK_EQ(sum_a, want_a);
        C17_CHECK_EQ(sum_b, want_b);
    }

    /* Mutual recursion */
    C17_CHECK_EQ(is_even(c17_opaque_int(24)), 1);
    C17_CHECK_EQ(is_odd(c17_opaque_int(24)), 0);
    C17_CHECK_EQ(is_even(c17_opaque_int(25)), 0);
    C17_CHECK_EQ(is_odd(c17_opaque_int(25)), 1);

    /* Self-recursion returning struct by value: fib(10) = 55, fib(11) = 89 */
    {
        struct pair f10 = fib_pair(c17_opaque_int(10));
        C17_CHECK_EQ(f10.a, 55);
        C17_CHECK_EQ(f10.b, 89);
    }

    C17_END();
}
