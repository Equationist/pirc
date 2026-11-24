/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdint.h>

uint64_t bench_run(void) {
    static const int32_t prog[16] = {
        0, 1, 2, 3, 4, 5, 6, 7,
        1, 3, 5, 7, 0, 2, 4, 6
    };
    uint64_t a = 1;
    uint64_t b = 7;
    int64_t pc = 0;

    for (int64_t step = 0; step < 2000000; ++step) {
        int32_t op = prog[pc & 15];
        pc += 1;
        switch (op) {
            case 0: a += 13; break;
            case 1: a ^= (b + 5); break;
            case 2: b += a * 3; break;
            case 3: a = (a << 3) | (a >> 61); break;
            case 4: b ^= (a >> 2); break;
            case 5: a -= 7; break;
            case 6: b += 29; break;
            case 7: a += (b & 255); break;
            default: a ^= 0xFF; break;
        }
    }
    return a ^ b;
}
