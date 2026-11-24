/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_COMMON_VEC_H
#define PIR_COMMON_VEC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Type-safe, zero-dependency C99 dynamic array (clean under -Wconversion).
 * Usage:
 *   typedef PIR_VEC(uint32_t) U32Vec;
 *   U32Vec v = {0};
 *   PIR_VEC_PUSH(&v, 42u);
 *   PIR_VEC_FREE(&v);
 */
#define PIR_VEC(T)  \
    struct {        \
        T       *data; \
        uint32_t len;  \
        uint32_t cap;  \
    }

static inline void *pir_vec_grow_impl(void *ptr, uint32_t *cap, size_t elem_size) {
    uint32_t next_cap = (*cap == 0u) ? 8u : (*cap * 2u);
    void    *next     = realloc(ptr, (size_t)next_cap * elem_size);
    if (!next) {
        fprintf(stderr, "pirc: out of memory growing vector (%u elems)\n", next_cap);
        abort();
    }
    *cap = next_cap;
    return next;
}

#define PIR_VEC_PUSH(vec, val)                                                   \
    do {                                                                         \
        if ((vec)->len == (vec)->cap) {                                          \
            (vec)->data = pir_vec_grow_impl((vec)->data, &(vec)->cap,            \
                                            sizeof(*(vec)->data));               \
        }                                                                        \
        (vec)->data[(vec)->len++] = (val);                                       \
    } while (0)

#define PIR_VEC_CLEAR(vec) \
    do {                   \
        (vec)->len = 0u;   \
    } while (0)

#define PIR_VEC_FREE(vec)    \
    do {                     \
        free((vec)->data);   \
        (vec)->data = NULL;  \
        (vec)->len  = 0u;    \
        (vec)->cap  = 0u;    \
    } while (0)

#endif /* PIR_COMMON_VEC_H */
