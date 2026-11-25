/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.3 — Real-world volatile optimizer bugs
 * (GCC, LLVM, QBE, CompCert):
 *   - volatile loads must NOT be CSE'd, GVN'd, or hoisted out of loops by LICM
 *   - volatile stores must NOT be eliminated by DSE even when overwritten
 *   - Discarded volatile reads must NOT be removed by DCE
 *   - volatile struct members & volatile pointer to non-volatile vs ptr-to-volatile
 */
#include <stdint.h>
#include "c17_test.h"

struct mmio_regs {
    volatile uint32_t status;
    volatile uint32_t data;
};

static uint32_t read_twice_sum(volatile uint32_t *p) {
    uint32_t a = *p;
    uint32_t b = *p;
    return a + b;
}

static void write_sequence(volatile uint32_t *p, uint32_t v1, uint32_t v2) {
    *p = v1;
    *p = v2;
}

static uint32_t volatile_loop_sum(volatile uint32_t *p, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) {
        s += *p;
    }
    return s;
}

int main(void) {
    C17_BEGIN("c67_bug_volatile_semantics",
              "volatile load/store preservation across GVN/LICM/DSE/DCE & structs");

    {
        volatile uint32_t reg = 15U;
        C17_CHECK_UEQ(read_twice_sum(&reg), 30U);
        write_sequence(&reg, 111U, 222U);
        C17_CHECK_UEQ(reg, 222U);
        /* Discarded volatile load must compile and execute cleanly */
        (void)reg;
        C17_CHECK_UEQ(volatile_loop_sum(&reg, c17_opaque_int(4)), 888U);
    }

    {
        struct mmio_regs m = { 1U, 2U };
        m.status = (uint32_t)c17_opaque_int(0x10);
        m.data = (uint32_t)c17_opaque_int(0x20);
        C17_CHECK_UEQ(m.status + m.data, 0x30U);
        m.status |= 0x05U;
        C17_CHECK_UEQ(m.status, 0x15U);
    }

    /* volatile pointer to non-volatile int vs pointer to volatile int */
    {
        int x = 10;
        int y = 20;
        int *volatile vp = &x;
        C17_CHECK_EQ(*vp, 10);
        vp = &y;
        C17_CHECK_EQ(*vp, 20);
    }

    C17_END();
}
