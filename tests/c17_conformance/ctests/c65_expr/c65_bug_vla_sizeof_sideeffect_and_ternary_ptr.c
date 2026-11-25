/*
 * C17 Conformance Test — Class C (§6.5.3.4 / §6.5.15 / §6.2.4 / §6.7.6.2)
 * Adversarial regressions for:
 *   1) sizeof(vla[side()]) MUST evaluate side() iff the operand has VM type,
 *      and MUST NOT evaluate side() when the indexed element is a fixed array
 *      (GCC PR43402 / Clang PR10722).
 *   2) 3D VLA indexing with runtime dimensions and pointer-to-VLA arithmetic.
 *   3) Backward goto exiting a VLA scope across 50,000 iterations (stack
 *      reclamation stress: a missing stackrestore overflows the stack).
 *   4) Conditional operator (?:) null pointer constant & qualifier merging
 *      (§6.5.15p6), verified via _Generic.
 */
#include "c17_test.h"
#include <stddef.h>
#include <stdint.h>

static int g_side_log = 0;
static int bump(int delta) {
    g_side_log += delta;
    return 0;
}

static size_t check_vla_sizeof(int n, int m, int *out_log) {
    int a[n][m];
    int b[n][4];
    int (*pa)[m] = a;
    int (*pb)[4] = b;

    g_side_log = 0;
    size_t s1 = sizeof(a[bump(1)]);     /* type int[m]: VM -> MUST eval (+1) */
    size_t s2 = sizeof(b[bump(10)]);    /* type int[4]: non-VM -> MUST NOT eval */
    size_t s3 = sizeof(*pa + bump(100));/* type int *: non-VM -> MUST NOT eval */
    size_t s4 = sizeof(pa[bump(2)]);    /* type int[m]: VM -> MUST eval (+2) */
    size_t s5 = sizeof(pb[bump(1000)]); /* type int[4]: non-VM -> MUST NOT eval */
    *out_log = g_side_log;
    return s1 + s2 + s3 + s4 + s5;
}

static int vla_3d_sum(int x, int y, int z) {
    int arr[x][y][z];
    for (int i = 0; i < x; i++)
        for (int j = 0; j < y; j++)
            for (int k = 0; k < z; k++)
                arr[i][j][k] = i * 100 + j * 10 + k;

    int sum = 0;
    for (int i = 0; i < x; i++) {
        int (*slice)[z] = arr[i];
        for (int j = 0; j < y; j++)
            for (int k = 0; k < z; k++)
                sum += slice[j][k];
    }
    return sum;
}

static int vla_goto_reclaim_stress(int iters, int len) {
    int i = 0;
    int acc = 0;
loop_top:
    if (i < iters) {
        int vla[len]; /* ~1KB per iter; 50k iters = 50MB if leaked! */
        vla[0] = i;
        vla[len - 1] = i ^ 0x55;
        acc += (vla[0] + vla[len - 1]) & 7;
        i++;
        goto loop_top;
    }
    return acc;
}

int main(void) {
    C17_BEGIN("c65_bug_vla_sizeof_sideeffect_and_ternary_ptr",
              "VLA sizeof side-effects, 3D VLA, goto VLA stack reclamation, ternary ptrs");

    /* 1. sizeof VM vs non-VM side effects */
    int log = -1;
    size_t tot = check_vla_sizeof(c17_opaque_int(3), c17_opaque_int(5), &log);
    C17_CHECK(log == 3);
    C17_CHECK(tot == (5 + 4 + 5 + 4) * sizeof(int) + sizeof(int *));

    /* 2. 3D VLA */
    int s3d = vla_3d_sum(c17_opaque_int(3), c17_opaque_int(4), c17_opaque_int(5));
    int expect = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 5; k++)
                expect += i * 100 + j * 10 + k;
    C17_CHECK(s3d == expect);

    /* 3. 50,000-iteration goto-out-of-VLA reclamation (50MB if leaked) */
    int acc = vla_goto_reclaim_stress(c17_opaque_int(50000), c17_opaque_int(256));
    int exp_acc = 0;
    for (int i = 0; i < 50000; i++)
        exp_acc += (i + (i ^ 0x55)) & 7;
    C17_CHECK(acc == exp_acc);

    /* 4. Conditional operator (?:) NPC & qualifier merging */
    int x = 42;
    int *ip = &x;
    void *vp = &x;
    const int *cip = &x;
    volatile int *vip = &x;
    int cond = c17_opaque_int(1);

    C17_CHECK(_Generic(cond ? ip : (void *)0, int *: 1, void *: 2, default: 3) == 1);
    C17_CHECK(_Generic(cond ? (void *)0 : ip, int *: 1, void *: 2, default: 3) == 1);
    C17_CHECK(_Generic(cond ? ip : 0,         int *: 1, void *: 2, default: 3) == 1);
    C17_CHECK(_Generic(cond ? ip : vp,        int *: 1, void *: 2, default: 3) == 2);
    C17_CHECK(_Generic(cond ? cip : vip, const volatile int *: 1, default: 2) == 1);
    C17_CHECK(_Generic(cond ? cip : ip,  const int *: 1, int *: 2, default: 3) == 1);

    C17_END();
}
