/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_COMMON_SYMSET_H
#define PIR_COMMON_SYMSET_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Lightweight C99 open-addressing string set (FNV-1a hash, power-of-two capacity).
 * Stores borrowed NUL-terminated string pointers (typically arena-allocated).
 */
typedef struct {
    const char **slots;
    uint32_t     cap;
    uint32_t     len;
} PirSymSet;

static inline uint32_t pir_fnv1a_str(const char *s) {
    uint32_t h = 2166136261u;
    for (const unsigned char *q = (const unsigned char *)s; *q; ++q) {
        h ^= (uint32_t)*q;
        h *= 16777619u;
    }
    return h;
}

static inline void pir_symset_init(PirSymSet *st, uint32_t hint) {
    uint32_t cap = 32u;
    while (cap < hint * 2u && cap < (1u << 24)) cap <<= 1u;
    st->slots = (const char **)calloc((size_t)cap, sizeof(const char *));
    if (!st->slots) abort();
    st->cap = cap;
    st->len = 0u;
}

static inline void pir_symset_clear(PirSymSet *st) {
    if (st->slots && st->cap > 0u) {
        memset(st->slots, 0, (size_t)st->cap * sizeof(const char *));
    }
    st->len = 0u;
}

static inline void pir_symset_destroy(PirSymSet *st) {
    free(st->slots);
    st->slots = NULL;
    st->cap   = 0u;
    st->len   = 0u;
}

static inline void pir_symset_grow(PirSymSet *st) {
    uint32_t ncap = (st->cap == 0u) ? 32u : (st->cap * 2u);
    const char **ns = (const char **)calloc((size_t)ncap, sizeof(const char *));
    if (!ns) abort();
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0u; i < st->cap; ++i) {
        if (!st->slots[i]) continue;
        uint32_t idx = pir_fnv1a_str(st->slots[i]) & mask;
        while (ns[idx]) idx = (idx + 1u) & mask;
        ns[idx] = st->slots[i];
    }
    free(st->slots);
    st->slots = ns;
    st->cap   = ncap;
}

static inline bool pir_symset_contains(const PirSymSet *st, const char *name) {
    if (!name || st->cap == 0u) return false;
    uint32_t mask = st->cap - 1u;
    uint32_t idx  = pir_fnv1a_str(name) & mask;
    while (st->slots[idx]) {
        if (strcmp(st->slots[idx], name) == 0) return true;
        idx = (idx + 1u) & mask;
    }
    return false;
}

/* Returns true if newly inserted, false if already present. */
static inline bool pir_symset_insert(PirSymSet *st, const char *name) {
    if (!name) return true;
    if (st->cap == 0u || (st->len + 1u) * 2u >= st->cap) {
        pir_symset_grow(st);
    }
    uint32_t mask = st->cap - 1u;
    uint32_t idx  = pir_fnv1a_str(name) & mask;
    while (st->slots[idx]) {
        if (strcmp(st->slots[idx], name) == 0) return false;
        idx = (idx + 1u) & mask;
    }
    st->slots[idx] = name;
    st->len++;
    return true;
}

#endif /* PIR_COMMON_SYMSET_H */
