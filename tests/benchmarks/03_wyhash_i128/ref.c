/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

__extension__ typedef unsigned __int128 u128_t;

static inline uint64_t wymix(uint64_t a, uint64_t b) {
    u128_t r = (u128_t)a * (u128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
}

uint64_t bench_run(void) {
    uint64_t seed = 0xA0761D6478BD642FULL;
    const uint64_t s1 = 0xE7037ED1A0B428DBULL;
    const uint64_t s2 = 0x8EBC6AF09C88C6E3ULL;
    for (int64_t i = 0; i < 2000000; ++i) {
        uint64_t a = seed ^ s1;
        uint64_t b = (uint64_t)i ^ s2;
        seed = wymix(a, b);
    }
    return seed;
}
