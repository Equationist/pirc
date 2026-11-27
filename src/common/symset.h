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
    uint32_t target = (hint > (1u << 23)) ? (1u << 24) : (hint * 2u);
    while (cap < target && cap < (1u << 24)) cap <<= 1u;
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
    if (st->cap >= (1u << 28)) abort();
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

/* Returns true if newly inserted, false if already present (or NULL). */
static inline bool pir_symset_insert(PirSymSet *st, const char *name) {
    if (!name) return false;
    if (st->cap == 0u || st->len >= st->cap / 2u) {
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

/*
 * Lightweight C99 open-addressing string -> uint32_t map (FNV-1a hash, power-of-two capacity).
 */
typedef struct {
    const char **keys;
    uint32_t    *vals;
    uint32_t     cap;
    uint32_t     len;
} PirSymMap;

static inline void pir_symmap_init(PirSymMap *m, uint32_t hint) {
    uint32_t cap = 32u;
    uint32_t target = (hint > (1u << 23)) ? (1u << 24) : (hint * 2u);
    while (cap < target && cap < (1u << 24)) cap <<= 1u;
    m->keys = (const char **)calloc((size_t)cap, sizeof(const char *));
    m->vals = (uint32_t *)malloc((size_t)cap * sizeof(uint32_t));
    if (!m->keys || !m->vals) abort();
    m->cap = cap;
    m->len = 0u;
}

static inline void pir_symmap_clear(PirSymMap *m) {
    if (m->keys && m->cap > 0u) {
        memset(m->keys, 0, (size_t)m->cap * sizeof(const char *));
    }
    m->len = 0u;
}

static inline void pir_symmap_destroy(PirSymMap *m) {
    free(m->keys);
    free(m->vals);
    m->keys = NULL;
    m->vals = NULL;
    m->cap  = 0u;
    m->len  = 0u;
}

static inline void pir_symmap_grow(PirSymMap *m) {
    if (m->cap >= (1u << 28)) abort();
    uint32_t ncap = (m->cap == 0u) ? 32u : (m->cap * 2u);
    const char **nk = (const char **)calloc((size_t)ncap, sizeof(const char *));
    uint32_t    *nv = (uint32_t *)malloc((size_t)ncap * sizeof(uint32_t));
    if (!nk || !nv) abort();
    uint32_t mask = ncap - 1u;
    for (uint32_t i = 0u; i < m->cap; ++i) {
        if (!m->keys[i]) continue;
        uint32_t idx = pir_fnv1a_str(m->keys[i]) & mask;
        while (nk[idx]) idx = (idx + 1u) & mask;
        nk[idx] = m->keys[i];
        nv[idx] = m->vals[i];
    }
    free(m->keys);
    free(m->vals);
    m->keys = nk;
    m->vals = nv;
    m->cap  = ncap;
}

/* Inserts key->val if absent (first-wins, matching linear scans). */
static inline void pir_symmap_put_if_absent(PirSymMap *m, const char *key, uint32_t val) {
    if (!key) return;
    if (m->cap == 0u || m->len >= m->cap / 2u) {
        pir_symmap_grow(m);
    }
    uint32_t mask = m->cap - 1u;
    uint32_t idx  = pir_fnv1a_str(key) & mask;
    while (m->keys[idx]) {
        if (strcmp(m->keys[idx], key) == 0) return;
        idx = (idx + 1u) & mask;
    }
    m->keys[idx] = key;
    m->vals[idx] = val;
    m->len++;
}

static inline bool pir_symmap_get(const PirSymMap *m, const char *key, uint32_t *out_val) {
    if (!key || m->cap == 0u) return false;
    uint32_t mask = m->cap - 1u;
    uint32_t idx  = pir_fnv1a_str(key) & mask;
    while (m->keys[idx]) {
        if (strcmp(m->keys[idx], key) == 0) {
            if (out_val) *out_val = m->vals[idx];
            return true;
        }
        idx = (idx + 1u) & mask;
    }
    return false;
}

#endif /* PIR_COMMON_SYMSET_H */
