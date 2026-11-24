/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_M7_C_ABI_COMMON_H
#define PIR_M7_C_ABI_COMMON_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__extension__ typedef __int128 pir_i128;
__extension__ typedef unsigned __int128 pir_u128;

typedef float    v4f32_t __attribute__((vector_size(16)));
typedef double   v2f64_t __attribute__((vector_size(16)));
typedef int32_t  v4i32_t __attribute__((vector_size(16)));
typedef uint8_t  v16u8_t __attribute__((vector_size(16)));

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_EQ_I64(got, exp)                                                 \
    do {                                                                       \
        int64_t _g = (int64_t)(got);                                           \
        int64_t _e = (int64_t)(exp);                                           \
        if (_g != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == %lld (expected %lld)\n",        \
                    __FILE__, __LINE__, #got, (long long)_g, (long long)_e);   \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_EQ_U64(got, exp)                                                 \
    do {                                                                       \
        uint64_t _g = (uint64_t)(got);                                         \
        uint64_t _e = (uint64_t)(exp);                                         \
        if (_g != _e) {                                                        \
            fprintf(stderr, "FAIL %s:%d: %s == 0x%llx (expected 0x%llx)\n",    \
                    __FILE__, __LINE__, #got,                                  \
                    (unsigned long long)_g, (unsigned long long)_e);           \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define CHECK_EQ_F64(got, exp)                                                 \
    do {                                                                       \
        double _g = (double)(got);                                             \
        double _e = (double)(exp);                                             \
        if (fabs(_g - _e) > 1e-5) {                                            \
            fprintf(stderr, "FAIL %s:%d: %s == %.6f (expected %.6f)\n",        \
                    __FILE__, __LINE__, #got, _g, _e);                         \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#endif /* PIR_M7_C_ABI_COMMON_H */
