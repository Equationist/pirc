/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 / §6.8 — Real-world 2025/2026 LLVM & GCC bugs:
 *   - LLVM #244613: `(reduce_or ^ reduce_umax) >> 7` on uint8_t arrays
 *   - LLVM #195088: DFA jump-threading (6-state switch-in-while state machine)
 *   - LLVM #220904: MemCpyOpt-style struct copy + memcpy + volatile checksum
 */
#include <stdint.h>
#include <string.h>
#include "c17_test.h"

/* LLVM #244613 pattern: byte OR-reduction XOR byte UMAX-reduction >> 7 */
static uint8_t llvm_244613_reduce(const uint8_t *a, int n) {
    uint8_t r_or = 0;
    uint8_t r_max = 0;
    for (int i = 0; i < n; i++) {
        r_or |= a[i];
        if (a[i] > r_max) {
            r_max = a[i];
        }
    }
    return (uint8_t)((r_or ^ r_max) >> 7);
}

/* LLVM #195088 pattern: DFA state machine with switch-in-while */
static int run_dfa(const char *s) {
    int state = 0;
    int score = 0;
    for (const char *p = s; *p != '\0'; p++) {
        char ch = *p;
        switch (state) {
        case 0:
            if (ch == 'a') { state = 1; score += 1; }
            else if (ch == 'b') { state = 2; score += 2; }
            else { state = 0; }
            break;
        case 1:
            if (ch == 'b') { state = 3; score += 10; }
            else if (ch == 'a') { state = 1; score += 3; }
            else { state = 0; score += 1; }
            break;
        case 2:
            if (ch == 'a') { state = 4; score += 20; }
            else { state = 0; }
            break;
        case 3:
            if (ch == 'c') { state = 5; score += 100; }
            else if (ch == 'a') { state = 1; score += 5; }
            else { state = 2; }
            break;
        case 4:
            if (ch == 'c') { state = 5; score += 200; }
            else { state = 1; score += 7; }
            break;
        case 5:
            score += 1000;
            state = (ch == 'a') ? 1 : 0;
            break;
        default:
            state = 0;
            break;
        }
    }
    return score + state;
}

/* LLVM #220904 pattern: struct copy + memcpy + volatile sink */
struct pkt {
    uint32_t a, b, c, d, e;
};

static volatile uint32_t g_sink220904 = 0;

static uint32_t llvm_220904_copy(struct pkt src, int flag) {
    struct pkt tmp;
    struct pkt dst;
    memcpy(&tmp, &src, sizeof(tmp));
    if (flag) {
        tmp.c ^= 0x55AAU;
    }
    g_sink220904 = tmp.a;
    dst = tmp;
    tmp.c = 0; /* must NOT affect dst.c! */
    return dst.a + dst.b + dst.c + dst.d + dst.e + g_sink220904;
}

int main(void) {
    C17_BEGIN("c65_bug_llvm_244613_220904_195088",
              "LLVM #244613 byte reduce, #195088 DFA switch, #220904 memcpy/struct");

    /* LLVM #244613: both r_or and r_max have bit 7 set -> (r_or ^ r_max) >> 7 == 0!
     * And when all < 0x80 -> bit 7 is 0 -> 0! */
    {
        uint8_t v1[16] = { 0x80, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40,
                           0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
        /* r_or = 0xFF, r_max = 0x80 -> (0xFF ^ 0x80) = 0x7F -> >> 7 == 0 */
        C17_CHECK_EQ(llvm_244613_reduce(v1, c17_opaque_int(16)), 0);

        uint8_t v2[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x00 };
        /* r_or = 0x7F, r_max = 0x40 -> (0x7F ^ 0x40) = 0x3F -> >> 7 == 0 */
        C17_CHECK_EQ(llvm_244613_reduce(v2, c17_opaque_int(8)), 0);
    }

    /* LLVM #195088 DFA trace on "abcaabacx":
     * 'a': 0->1 (+1)
     * 'b': 1->3 (+10)
     * 'c': 3->5 (+100)
     * 'a': 5->1 (+1000)
     * 'a': 1->1 (+3)
     * 'b': 1->3 (+10)
     * 'a': 3->1 (+5)
     * 'c': 1->0 (+1)
     * 'x': 0->0 (+0)
     * score = 1130, state = 0 -> 1130
     */
    C17_CHECK_EQ(run_dfa("abcaabacx"), 1130);
    /* "bac!": 'b':0->2(+2), 'a':2->4(+20), 'c':4->5(+200), '!':5->0(+1000) -> 1222 */
    C17_CHECK_EQ(run_dfa("bac!"), 1222);

    /* LLVM #220904 */
    {
        struct pkt p = { 10, 20, 0x00FFU, 40, 50 };
        uint32_t got = llvm_220904_copy(p, c17_opaque_int(1));
        uint32_t want = 10U + 20U + (0x00FFU ^ 0x55AAU) + 40U + 50U + 10U;
        C17_CHECK_UEQ(got, want);
    }

    C17_END();
}
