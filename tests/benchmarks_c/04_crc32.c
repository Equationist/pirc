/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Table-driven IEEE CRC-32 over 4 MB (table lookup + loop-carried dependency, pure C99). */
#include "bench_c_harness.h"

static uint32_t crc_table[256];
static int crc_init_done = 0;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256u; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            if (c & 1u)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        crc_table[i] = c;
    }
    crc_init_done = 1;
}

static uint32_t crc32_compute(const uint8_t * restrict data, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        c = crc_table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

static uint64_t bench_kernel(void) {
    if (!crc_init_done) crc32_init();
    const size_t N = 4u * 1024u * 1024u;
    static uint8_t *buf = NULL;
    if (!buf) {
        buf = (uint8_t *)malloc(N);
        uint32_t x = 0x12345678u;
        for (size_t i = 0; i < N; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            buf[i] = (uint8_t)x;
        }
    }
    uint32_t h1 = crc32_compute(buf, N);
    uint32_t h2 = crc32_compute(buf + 13, N - 13);
    return ((uint64_t)h1 << 32) | (uint64_t)h2;
}
