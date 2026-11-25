/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.2.1 / §6.7.5 — Real-world struct layout & alignment bugs:
 *   - Over-aligned _Alignas(32) & _Alignas(64) structs on stack, in arrays,
 *     and passed by value
 *   - Struct tail padding & nested anonymous struct/union + bitfield + array
 *   - Dynamic malloc of flexible array member with alignment verification
 */
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "c17_test.h"

struct align32_box {
    _Alignas(32) int x;
    char tag;
};

struct align64_box {
    _Alignas(64) uint64_t val;
    uint8_t mark;
};

struct tail_pad {
    int a;
    char b;
};

struct nested_complex {
    int header;
    union {
        struct {
            unsigned lo : 5;
            unsigned hi : 3;
            uint8_t bytes[3];
        };
        uint32_t raw;
    };
    short footer;
};

struct fam_doubles {
    size_t count;
    char tag;
    double values[];
};

static int read_align32(struct align32_box b) {
    return b.x + (int)b.tag;
}

int main(void) {
    C17_BEGIN("c67_bug_struct_layout_padding",
              "_Alignas(32/64) structs, tail padding, nested anon union/bf, FAM");

    /* _Alignas(32) & _Alignas(64) size, alignment, array stride, stack align, by-value pass */
    {
        C17_CHECK_EQ(_Alignof(struct align32_box), 32);
        C17_CHECK_EQ(sizeof(struct align32_box), 32);
        C17_CHECK_EQ(_Alignof(struct align64_box), 64);
        C17_CHECK_EQ(sizeof(struct align64_box), 64);

        struct align32_box arr32[3];
        for (int i = 0; i < 3; i++) {
            C17_CHECK_UEQ((uintptr_t)&arr32[i] % 32U, 0U);
            arr32[i].x = c17_opaque_int(100 * (i + 1));
            arr32[i].tag = (char)(i + 1);
        }
        C17_CHECK_EQ(read_align32(arr32[0]), 101);
        C17_CHECK_EQ(read_align32(arr32[2]), 303);

        struct align64_box b64;
        C17_CHECK_UEQ((uintptr_t)&b64 % 64U, 0U);
        b64.val = c17_opaque_ull(0xCAFEBABE12345678ULL);
        b64.mark = 0x5A;
        C17_CHECK_UEQ(b64.val, 0xCAFEBABE12345678ULL);
        C17_CHECK_EQ(b64.mark, 0x5A);
    }

    /* Tail padding size & offsetof */
    {
        C17_CHECK_EQ(offsetof(struct tail_pad, a), 0);
        C17_CHECK_EQ(offsetof(struct tail_pad, b), sizeof(int));
        C17_CHECK(sizeof(struct tail_pad) >= sizeof(int) + 1);
        C17_CHECK_EQ(sizeof(struct tail_pad) % _Alignof(struct tail_pad), 0);
    }

    /* Nested anonymous union containing anonymous struct with bitfield + array */
    {
        struct nested_complex nc = { 0 };
        nc.header = c17_opaque_int(42);
        nc.lo = 21;
        nc.hi = 5;
        nc.bytes[0] = 0x11;
        nc.bytes[1] = 0x22;
        nc.bytes[2] = 0x33;
        nc.footer = -7;

        struct nested_complex copy = nc;
        C17_CHECK_EQ(copy.header, 42);
        C17_CHECK_EQ(copy.lo, 21);
        C17_CHECK_EQ(copy.hi, 5);
        C17_CHECK_EQ(copy.bytes[0], 0x11);
        C17_CHECK_EQ(copy.bytes[1], 0x22);
        C17_CHECK_EQ(copy.bytes[2], 0x33);
        C17_CHECK_EQ(copy.footer, -7);
    }

    /* Dynamically allocated FAM with double alignment */
    {
        size_t n = (size_t)c17_opaque_int(6);
        struct fam_doubles *p = (struct fam_doubles *)malloc(
            sizeof(struct fam_doubles) + n * sizeof(double));
        C17_CHECK(p != NULL);
        if (p) {
            p->count = n;
            p->tag = 'D';
            C17_CHECK_UEQ((uintptr_t)&p->values[0] % _Alignof(double), 0U);
            double sum = 0.0;
            for (size_t i = 0; i < n; i++) {
                p->values[i] = (double)(i + 1) * 1.5;
                sum += p->values[i];
            }
            /* (1+2+3+4+5+6) * 1.5 = 31.5 */
            C17_CHECK_DBL(sum, 31.5, 0.0);
            C17_CHECK_EQ(p->tag, 'D');
            free(p);
        }
    }

    C17_END();
}
