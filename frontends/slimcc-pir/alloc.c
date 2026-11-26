#include "slimcc.h"

#define FREE_THRESHOLD (100 * 1024)
#define ARENA_POOL_SIZE 8160

struct Pool {
  Pool *next;
  size_t oversized;
  char buf[ARENA_POOL_SIZE];
};

Arena ast_arena;
Arena cc1_arena;
Arena pp_arena;
bool free_alloc = EAGER_FREE;

static Pool *pool_freelist;

bool check_mem_usage(void) {
  struct rusage stat;
  getrusage(RUSAGE_SELF, &stat);
  return stat.ru_maxrss > FREE_THRESHOLD;
}

static Pool *new_pool(void) {
  Pool *p;
  if (pool_freelist) {
    p = pool_freelist;
    pool_freelist = p->next;
  } else {
    p = malloc(sizeof(Pool));
    if (!p)
      error("out of memory");
  }

#if defined(USE_ASAN)
  __asan_poison_memory_region(&p->buf, ARENA_POOL_SIZE);
#endif
  p->next = NULL;
  p->oversized = false;
  return p;
}

static void *allocate_oversized(Arena *arena, size_t aligned_sz, bool clear) {
  size_t hdr = (size_t)offsetof(Pool, buf);
  if (aligned_sz > SIZE_MAX - hdr)
    error("out of memory");
  Pool *p = malloc(hdr + aligned_sz);
  if (!p)
    error("out of memory");
  p->oversized = true;
  /* Link behind arena->head so arena->cur remains a standard reusable pool. */
  p->next = arena->head;
  arena->head = p;
  void *ptr = p->buf;
  if (clear)
    memset(ptr, 0, aligned_sz);
  return ptr;
}

static void *allocate(Arena *arena, size_t sz, bool clear) {
  size_t aligned_sz = (sz + 15) & -16LL;

  if (aligned_sz > ARENA_POOL_SIZE)
    return allocate_oversized(arena, aligned_sz, clear);

  void *ptr;
  if ((arena->used + aligned_sz) <= ARENA_POOL_SIZE) {
    ptr = &arena->cur->buf[arena->used];
    arena->used += aligned_sz;
  } else {
    arena->cur = arena->cur->next = new_pool();
    ptr = &arena->cur->buf;
    arena->used = aligned_sz;
  }

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, aligned_sz);
#endif

#if defined(USE_TYSAN)
  memset(ptr, 0, aligned_sz);
#else
  if (clear)
    memset(ptr, 0, aligned_sz);
#endif
  return ptr;
}

char *arena_format(Arena *arena, const char *fmt, ...) {
  void *ptr = &arena->cur->buf[arena->used];
  size_t n = ARENA_POOL_SIZE - arena->used;

#if defined(USE_ASAN)
  __asan_unpoison_memory_region(ptr, n);
#endif

  va_list ap;
  va_start(ap, fmt);

  int needed = vsnprintf(ptr, n, fmt, ap);
  va_end(ap);
  if (needed < 0)
    internal_error();
  size_t sz = 1 + (size_t)needed;
  size_t aligned_sz = (sz + 15) & -16LL;

  if (aligned_sz <= n) {
    arena->used += aligned_sz;
#if defined(USE_ASAN)
    __asan_poison_memory_region((char *)ptr + aligned_sz, ARENA_POOL_SIZE - arena->used);
#endif
    return ptr;
  }

#if defined(USE_ASAN)
  __asan_poison_memory_region(ptr, n);
#endif

  if (aligned_sz > ARENA_POOL_SIZE) {
    ptr = allocate_oversized(arena, aligned_sz, false);
  } else {
    arena->cur = arena->cur->next = new_pool();
    ptr = &arena->cur->buf;
    arena->used = aligned_sz;
#if defined(USE_ASAN)
    __asan_unpoison_memory_region(ptr, aligned_sz);
#endif
  }

  va_start(ap, fmt);
  vsnprintf(ptr, aligned_sz, fmt, ap);
  va_end(ap);
  return ptr;
}

char *arena_strdup(Arena *arena, const char *str) {
  size_t len = 1 + strlen(str);
  return memcpy(arena_malloc(arena, len), str, len);
}

char *arena_copy_string(Arena *arena, const char *src, size_t slen) {
  char *p = memcpy(arena_malloc(arena, slen + 1), src, slen);
  p[slen] = '\0';
  return p;
}

void arena_on(Arena *arena) {
  arena->head = arena->cur = new_pool();
  arena->used = 0;
}

void arena_off(Arena *arena) {
  if (free_alloc) {
    for (Pool *p = arena->head; p;) {
      Pool *tmp = p;
      p = p->next;
      free(tmp);
    }
  } else {
    for (Pool *p = arena->head; p;) {
      Pool *tmp = p;
      p = p->next;
      if (tmp->oversized) {
        free(tmp);
      } else {
        tmp->next = pool_freelist;
        pool_freelist = tmp;
      }
    }
  }
  arena->head = arena->cur = NULL;
}

void *arena_malloc(Arena *a, size_t sz) {
  return allocate(a, sz, false);
}

void *arena_calloc(Arena *a, size_t sz) {
  return allocate(a, sz, true);
}
