/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef PIR_COMMON_ARENA_H
#define PIR_COMMON_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct PirArenaBlock PirArenaBlock;

typedef struct {
    PirArenaBlock *head;
    size_t         default_block_size;
    size_t         total_allocated;
} PirArena;

void   pir_arena_init(PirArena *arena, size_t block_size);
void   pir_arena_destroy(PirArena *arena);
void  *pir_arena_alloc_aligned(PirArena *arena, size_t size, size_t align);
void  *pir_arena_alloc(PirArena *arena, size_t size);
char  *pir_arena_strdup(PirArena *arena, const char *s);
char  *pir_arena_strndup(PirArena *arena, const char *s, size_t len);

#define PIR_ARENA_NEW(arena, T) \
    ((T *)pir_arena_alloc_aligned((arena), sizeof(T), sizeof(void *)))

#define PIR_ARENA_NEW_ARRAY(arena, T, count) \
    ((T *)pir_arena_alloc_aligned((arena), sizeof(T) * (size_t)(count), sizeof(void *)))

#endif /* PIR_COMMON_ARENA_H */
