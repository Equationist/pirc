/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2p5 — Integer promotions on switch
 * controlling expressions (signed/unsigned char, short, _Bool).
 */
#include "c17_test.h"

int main(void) {
    C17_BEGIN("c68_bug_switch_int_promotions",
              "C17 §6.8.4.2p5 integer promotions on switch controlling expressions");

    volatile signed char sc = -1;
    int r = 0;
    switch (sc) {
    case 255: r = 1; break;
    default:  r = 2; break;
    }
    C17_CHECK_EQ(r, 2);

    volatile unsigned char uc = 255;
    r = 0;
    switch (uc) {
    case -1:  r = 1; break;
    case 255: r = 3; break;
    default:  r = 2; break;
    }
    C17_CHECK_EQ(r, 3);

    volatile short sh = -1;
    r = 0;
    switch (sh) {
    case 65535: r = 1; break;
    case -1:    r = 4; break;
    default:    r = 2; break;
    }
    C17_CHECK_EQ(r, 4);

    volatile unsigned short ush = 65535;
    r = 0;
    switch (ush) {
    case -1:    r = 1; break;
    case 65535: r = 5; break;
    default:    r = 2; break;
    }
    C17_CHECK_EQ(r, 5);

    volatile _Bool bv = 1;
    r = 0;
    switch (bv) {
    case 2:  r = 1; break;
    case 1:  r = 6; break;
    default: r = 2; break;
    }
    C17_CHECK_EQ(r, 6);

    C17_END();
}
