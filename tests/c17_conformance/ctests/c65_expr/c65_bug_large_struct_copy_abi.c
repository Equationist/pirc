/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.2.2 / §6.5.16.1 — Legendary sret aliasing &
 * large-struct copy bugs (GCC, LLVM, TCC, chibicc, QBE):
 *   - `s = reverse_big(s)` where s is >16B (sret hidden pointer): writing into
 *     return buffer MUST NOT clobber the by-value input `s`!
 *   - `s = reverse_via_ptr(&s)` where return destination aliases a pointer arg!
 *   - Large structs (24B, 64B, 257B, 1024B) passed, returned, assigned, and
 *     selected via ternary `cond ? a : b`
 */
#include <stdint.h>
#include "c17_test.h"

struct big32 {
    int a[8];
};

struct big257 {
    uint8_t data[257];
};

/* Classic sret-aliasing trap: reverses fields of `in` into returned struct.
 * If caller passes `&s` as both sret output AND input without isolating copy,
 * writing `r.a[0] = in.a[7]` clobbers `in.a[0]` before `r.a[7] = in.a[0]`! */
static struct big32 reverse_byval(struct big32 in) {
    struct big32 r;
    for (int i = 0; i < 8; i++) {
        r.a[i] = in.a[7 - i];
    }
    return r;
}

/* Even nastier sret-aliasing trap: callee takes pointer `&s` AND returns struct
 * assigned back to `s`: `s = reverse_byptr(&s);`!
 * In C17, the assignment `s = f(&s)` MUST NOT corrupt `*p` while `f` is still
 * computing its return value! */
static struct big32 reverse_byptr(const struct big32 *p) {
    struct big32 r;
    for (int i = 0; i < 8; i++) {
        r.a[i] = p->a[7 - i];
    }
    return r;
}

static struct big257 rotate_big257(struct big257 in, int shift) {
    struct big257 r;
    for (int i = 0; i < 257; i++) {
        r.data[i] = in.data[(i + shift) % 257];
    }
    return r;
}

int main(void) {
    C17_BEGIN("c65_bug_large_struct_copy_abi",
              "Aliased sret `s = f(s)` and `s = f(&s)`, 257B struct pass/ret/ternary");

    /* Aliased sret by-value: s = reverse_byval(s) */
    {
        struct big32 s = { { 10, 20, 30, 40, 50, 60, 70, 80 } };
        s = reverse_byval(s);
        for (int i = 0; i < 8; i++) {
            C17_CHECK_EQ(s.a[i], (8 - i) * 10);
        }
    }

    /* Aliased sret by-pointer: s = reverse_byptr(&s) */
    {
        struct big32 s = { { 1, 2, 3, 4, 5, 6, 7, 8 } };
        s = reverse_byptr(&s);
        for (int i = 0; i < 8; i++) {
            C17_CHECK_EQ(s.a[i], 8 - i);
        }
    }

    /* 257-byte odd-size large struct self-returning + ternary */
    {
        struct big257 b;
        for (int i = 0; i < 257; i++) {
            b.data[i] = (uint8_t)(i & 0xFF);
        }
        b = rotate_big257(b, c17_opaque_int(13));
        C17_CHECK_EQ(b.data[0], 13);
        C17_CHECK_EQ(b.data[243], 256 & 0xFF);
        C17_CHECK_EQ(b.data[244], 0);
        C17_CHECK_EQ(b.data[256], 12);

        struct big257 zero = { { 0 } };
        struct big257 chosen = c17_opaque_int(1) ? b : zero;
        C17_CHECK_EQ(chosen.data[0], 13);
        C17_CHECK_EQ(chosen.data[256], 12);
    }

    /* Chained rvalue struct subscripting on large returned struct */
    {
        struct big32 s = { { 100, 200, 300, 400, 500, 600, 700, 800 } };
        int idx = c17_opaque_int(2);
        C17_CHECK_EQ(reverse_byval(s).a[idx], 600);
    }

    C17_END();
}
