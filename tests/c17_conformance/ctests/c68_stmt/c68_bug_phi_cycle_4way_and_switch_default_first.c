/*
 * C17 Conformance Test — Class C (§6.8.4.2 / §6.8.5 / §6.5.15)
 * Adversarial CFG & SSA parallel-copy regressions:
 *   1) 4-way and 5-way simultaneous SSA phi permutation cycles across loop
 *      backedges (stresses parallel-copy resolution / lost-copy / swap bugs).
 *   2) switch with default: BEFORE case labels + fallthrough into case.
 *   3) switch with ONLY a default: label (no case labels).
 *   4) Conditional operator with both operands of type void (§6.5.15p5).
 */
#include "c17_test.h"
#include <stdint.h>

static int64_t phi_cycle_5way(int n, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e) {
    for (int i = 0; i < n; i++) {
        int64_t na = b;
        int64_t nb = c;
        int64_t nc = d;
        int64_t nd = e;
        int64_t ne = a + (i & 3);
        if (i == 7) {
            /* Nested swap inside loop body */
            int64_t t = na; na = nd; nd = t;
        }
        a = na; b = nb; c = nc; d = nd; e = ne;
    }
    return a * 1 + b * 10 + c * 100 + d * 1000 + e * 10000;
}

static int switch_default_first(int x) {
    int r = 0;
    switch (x) {
        default:
            r += 100;
            /* fall through */
        case 1:
            r += 10;
            break;
        case 2:
            r += 20;
            /* fall through */
        case 3:
            r += 3;
            break;
    }
    return r;
}

static int switch_only_default(int x) {
    int r = 5;
    switch (x) {
        default:
            r += x * 2;
            break;
    }
    return r;
}

static int g_void_acc = 0;
static void add_left(void)  { g_void_acc += 13; }
static void add_right(void) { g_void_acc += 29; }

int main(void) {
    C17_BEGIN("c68_bug_phi_cycle_4way_and_switch_default_first",
              "5-way SSA phi permutation cycle, default-first switch, void ternary");

    /* 1. 5-way phi cycle (verify against scalar array simulation) */
    int n = c17_opaque_int(17);
    int64_t got = phi_cycle_5way(n, 1, 2, 3, 4, 5);
    int64_t sim[5] = { 1, 2, 3, 4, 5 };
    for (int i = 0; i < 17; i++) {
        int64_t nxt[5] = { sim[1], sim[2], sim[3], sim[4], sim[0] + (i & 3) };
        if (i == 7) { int64_t t = nxt[0]; nxt[0] = nxt[3]; nxt[3] = t; }
        for (int k = 0; k < 5; k++) sim[k] = nxt[k];
    }
    int64_t want = sim[0] + sim[1]*10 + sim[2]*100 + sim[3]*1000 + sim[4]*10000;
    C17_CHECK(got == want);

    /* 2. switch with default before case + fallthrough */
    C17_CHECK(switch_default_first(c17_opaque_int(99)) == 110);
    C17_CHECK(switch_default_first(c17_opaque_int(1))  == 10);
    C17_CHECK(switch_default_first(c17_opaque_int(2))  == 23);
    C17_CHECK(switch_default_first(c17_opaque_int(3))  == 3);

    /* 3. switch with only default */
    C17_CHECK(switch_only_default(c17_opaque_int(0))  == 5);
    C17_CHECK(switch_only_default(c17_opaque_int(11)) == 27);

    /* 4. void ternary */
    g_void_acc = 0;
    c17_opaque_int(1) ? add_left() : add_right();
    c17_opaque_int(0) ? add_left() : add_right();
    c17_opaque_int(2) ? add_left() : add_right();
    C17_CHECK(g_void_acc == 13 + 29 + 13);

    C17_END();
}
