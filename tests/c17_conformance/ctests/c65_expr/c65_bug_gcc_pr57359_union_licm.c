/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5 — Real-world compiler bug regressions:
 *   - GCC PR57359: loop store motion / LICM across aliasing union & pointer
 *   - Partial sub-word store followed by full-word load (store-forwarding)
 *   - Byte-level modification through unsigned char * (C17 §6.5 p7)
 */
#include <stdint.h>
#include "c17_test.h"

union u57359 {
    short s;
    int i;
};

static int gcc_pr57359_loop(union u57359 *u, int *pi, int n) {
    int k;
    for (k = 0; k < n; k++) {
        u->s = 0;
        pi[k] = 1;
    }
    return u->i;
}

union word_bytes {
    uint32_t u32;
    uint16_t u16[2];
    uint8_t u8[4];
};

static uint32_t partial_store_then_load(uint32_t init, uint8_t b0, uint8_t b1) {
    union word_bytes wb;
    wb.u32 = init;
    wb.u8[0] = b0;
    wb.u8[1] = b1;
    return wb.u32;
}

static uint32_t uchar_alias_modify(uint32_t x, uint8_t new_byte) {
    uint32_t v = x;
    unsigned char *p = (unsigned char *)&v;
    /* Determine endianness dynamically so test is strictly portable */
    uint32_t probe = 0x01020304U;
    unsigned char *pp = (unsigned char *)&probe;
    if (pp[0] == 0x04U) {
        /* little-endian: p[0] is LSB */
        p[0] = new_byte;
    } else {
        /* big-endian: p[3] is LSB */
        p[3] = new_byte;
    }
    return v;
}

int main(void) {
    C17_BEGIN("c65_bug_gcc_pr57359_union_licm",
              "GCC PR57359 union store motion, store-forwarding, uchar aliasing");

    /* GCC PR57359: u->s = 0 and pi[0] = 1 alias on iteration 0.
     * On iteration 0: u->s = 0 then u->i = 1.
     * Buggy LICM hoists/sinks u->s = 0 out of the loop, clobbering u->i to 0. */
    {
        union u57359 u;
        u.i = 0x12345678;
        int res = gcc_pr57359_loop(&u, &u.i, c17_opaque_int(1));
        C17_CHECK_EQ(res, 1);
    }

    /* Multiple iterations where pi points to array starting at &u.i */
    {
        struct {
            union u57359 u;
            int tail[3];
        } box;
        box.u.i = 99;
        box.tail[0] = 0;
        box.tail[1] = 0;
        box.tail[2] = 0;
        int arr[4] = { 10, 20, 30, 40 };
        /* Non-aliasing case: u->s = 0 must take effect */
        int r1 = gcc_pr57359_loop(&box.u, arr, c17_opaque_int(4));
        C17_CHECK_EQ(box.u.s, 0);
        C17_CHECK_EQ(arr[0], 1);
        C17_CHECK_EQ(arr[3], 1);
        (void)r1;
    }

    /* Partial sub-word store followed by full-word load (store forwarding) */
    {
        uint32_t probe = 0x01020304U;
        unsigned char *pp = (unsigned char *)&probe;
        uint32_t got = partial_store_then_load(
            (uint32_t)c17_opaque_ull(0xAABBCCDDULL),
            (uint8_t)c17_opaque_int(0x11),
            (uint8_t)c17_opaque_int(0x22));
        if (pp[0] == 0x04U) {
            C17_CHECK_UEQ(got, 0xAABB2211U);
        } else {
            C17_CHECK_UEQ(got, 0x1122CCDDU);
        }
    }

    /* unsigned char * aliasing of uint32_t */
    {
        uint32_t got = uchar_alias_modify(
            (uint32_t)c17_opaque_ull(0x12345600ULL),
            (uint8_t)c17_opaque_int(0xAB));
        C17_CHECK_UEQ(got, 0x123456ABU);
    }

    C17_END();
}
