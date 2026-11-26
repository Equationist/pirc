/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.8.4.2 — Regression for case labels sharing the
 * same statement as `default:` (both `case X: default:` and `default: case X:`),
 * in both native PIR switches and large compare-chain switches.
 */
#include "c17_test.h"

static int sw_case_before_default(int x) {
    switch (x) {
    case 1:
    default:
        return 100;
    case 2:
        return 200;
    case 3:
    case 4:
        return 300;
    }
}

static int sw_default_before_case(int x) {
    switch (x) {
    case 10:
        return 10;
    default:
    case 20:
    case 30:
        return 25;
    case 40:
        return 40;
    }
}

static int sw_large_shared_default(long long x) {
    switch (x) {
    case 0:
        return 1;
    case 1000000000LL:
    default:
        return 77;
    case 2000000000LL:
        return 88;
    }
}

int main(void) {
    C17_CHECK(sw_case_before_default(1) == 100);
    C17_CHECK(sw_case_before_default(99) == 100);
    C17_CHECK(sw_case_before_default(2) == 200);
    C17_CHECK(sw_case_before_default(3) == 300);
    C17_CHECK(sw_case_before_default(4) == 300);

    C17_CHECK(sw_default_before_case(10) == 10);
    C17_CHECK(sw_default_before_case(20) == 25);
    C17_CHECK(sw_default_before_case(30) == 25);
    C17_CHECK(sw_default_before_case(-5) == 25);
    C17_CHECK(sw_default_before_case(40) == 40);

    C17_CHECK(sw_large_shared_default(0) == 1);
    C17_CHECK(sw_large_shared_default(1000000000LL) == 77);
    C17_CHECK(sw_large_shared_default(-999LL) == 77);
    C17_CHECK(sw_large_shared_default(2000000000LL) == 88);

    C17_END();
}
