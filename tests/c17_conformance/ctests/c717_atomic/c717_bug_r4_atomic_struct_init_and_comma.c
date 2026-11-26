/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.7.9 / §6.5.17 — Round 4 regressions:
 *   1. Local _Atomic struct initialization (lock-free 8B and lock-based 12B/24B)
 *   2. Reading a member of an _Atomic struct via comma expression `(0, *ap).member`
 */
#include "c17_test.h"
#include <stdatomic.h>

struct Pair8 {
    int a;
    int b;
};

struct Triple12 {
    int x;
    int y;
    int z;
};

struct Big24 {
    long long a;
    long long b;
    long long c;
};

static int test_local_atomic_inits(int base) {
    _Atomic struct Pair8 p8 = (struct Pair8){ base + 1, base + 2 };
    _Atomic struct Triple12 t12 = (struct Triple12){ base + 3, base + 4, base + 5 };
    _Atomic struct Big24 b24 = (struct Big24){ base + 10, base + 20, base + 30 };

    struct Pair8 r8 = atomic_load(&p8);
    struct Triple12 r12 = atomic_load(&t12);
    struct Big24 r24 = atomic_load(&b24);

    if (r8.a != base + 1 || r8.b != base + 2)
        return 1;
    if (r12.x != base + 3 || r12.y != base + 4 || r12.z != base + 5)
        return 2;
    if (r24.a != base + 10 || r24.b != base + 20 || r24.c != base + 30)
        return 3;

    _Atomic struct Pair8 *pp8 = &p8;
    _Atomic struct Triple12 *pt12 = &t12;
    int side = 0;
    int c1 = (side += 1, *pp8).b;
    int c2 = (side += 10, *pt12).z;
    if (side != 11 || c1 != base + 2 || c2 != base + 5)
        return 4;

    return 0;
}

int main(void) {
    C17_CHECK(test_local_atomic_inits(100) == 0);
    C17_END();
}
