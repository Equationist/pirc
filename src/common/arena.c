/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "common/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PirArenaBlock {
    PirArenaBlock *next;
    size_t         cap;
    size_t         used;
    unsigned char  data[];
};

static size_t aligned_offset(const unsigned char *base, size_t used, size_t align) {
    uintptr_t cur     = (uintptr_t)(base + used);
    uintptr_t mask    = (uintptr_t)(align - 1u);
    uintptr_t aligned = (cur + mask) & ~mask;
    return used + (size_t)(aligned - cur);
}

static PirArenaBlock *alloc_block(size_t cap) {
    if (cap > SIZE_MAX - sizeof(PirArenaBlock)) {
        fprintf(stderr, "pirc: arena block allocation overflow (%zu bytes)\n", cap);
        abort();
    }
    PirArenaBlock *blk = (PirArenaBlock *)malloc(sizeof(PirArenaBlock) + cap);
    if (!blk) {
        fprintf(stderr, "pirc: out of memory allocating arena block (%zu bytes)\n", cap);
        abort();
    }
    blk->next = NULL;
    blk->cap  = cap;
    blk->used = 0;
    return blk;
}

void pir_arena_init(PirArena *arena, size_t block_size) {
    arena->head               = NULL;
    arena->default_block_size = block_size > 0 ? block_size : 65536u;
    arena->total_allocated    = 0;
}

void pir_arena_destroy(PirArena *arena) {
    PirArenaBlock *cur = arena->head;
    while (cur) {
        PirArenaBlock *next = cur->next;
        free(cur);
        cur = next;
    }
    arena->head            = NULL;
    arena->total_allocated = 0;
}

void *pir_arena_alloc_aligned(PirArena *arena, size_t size, size_t align) {
    if (align == 0u || (align & (align - 1u)) != 0u) {
        fprintf(stderr, "pirc: arena alignment must be a power of two (got %zu)\n", align);
        abort();
    }
    if (size == 0u) {
        size = 1u;
    }
    if (size > SIZE_MAX - align) {
        fprintf(stderr, "pirc: arena allocation size overflow (%zu bytes)\n", size);
        abort();
    }

    if (arena->head) {
        size_t offset = aligned_offset(arena->head->data, arena->head->used, align);
        if (offset <= arena->head->cap && size <= arena->head->cap - offset) {
            void *ptr = arena->head->data + offset;
            memset(ptr, 0, size);
            arena->head->used       = offset + size;
            arena->total_allocated += size;
            return ptr;
        }
    }

    size_t need = size + align;
    size_t cap  = arena->default_block_size;
    if (need > cap) {
        cap = need;
    }

    PirArenaBlock *blk = alloc_block(cap);
    blk->next          = arena->head;
    arena->head        = blk;

    size_t offset           = aligned_offset(blk->data, 0u, align);
    void  *ptr              = blk->data + offset;
    memset(ptr, 0, size);
    blk->used               = offset + size;
    arena->total_allocated += size;
    return ptr;
}

void *pir_arena_alloc_array(PirArena *arena, size_t count, size_t elem_size, size_t align) {
    if (elem_size != 0u && count > SIZE_MAX / elem_size) {
        fprintf(stderr, "pirc: arena array allocation overflow (%zu * %zu)\n", count, elem_size);
        abort();
    }
    return pir_arena_alloc_aligned(arena, count * elem_size, align);
}

void *pir_arena_alloc(PirArena *arena, size_t size) {
    return pir_arena_alloc_aligned(arena, size, sizeof(void *));
}

char *pir_arena_strndup(PirArena *arena, const char *s, size_t len) {
    char *copy = (char *)pir_arena_alloc_aligned(arena, len + 1u, 1u);
    if (len > 0u) {
        memcpy(copy, s, len);
    }
    copy[len] = '\0';
    return copy;
}

char *pir_arena_strdup(PirArena *arena, const char *s) {
    return pir_arena_strndup(arena, s, strlen(s));
}
