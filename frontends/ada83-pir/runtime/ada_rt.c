/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*
 * Minimal C99 Ada 83 Runtime for diana2pir + pirc.
 * Provides:
 *   - Standard Ada exception tags & unhandled exception handler
 *   - Text_IO (Character, String, Integer_IO, Float_IO)
 *   - Calendar (Clock in nanoseconds via CLOCK_MONOTONIC)
 *   - Integer/Float exponentiation helpers with optional overflow checks
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* External PIR shadow-stack unwind entrypoint from runtime/pir_rt.c */
extern void __pir_unwind_resume(void *exn);

/* Predefined Standard / IO_Exceptions / Calendar exception identity tags.
 * The address of each string serves as the unique exception pointer identity. */
const char __ada_exc_constraint_error[] = "CONSTRAINT_ERROR";
const char __ada_exc_numeric_error[]    = "NUMERIC_ERROR";
const char __ada_exc_program_error[]    = "PROGRAM_ERROR";
const char __ada_exc_storage_error[]    = "STORAGE_ERROR";
const char __ada_exc_tasking_error[]    = "TASKING_ERROR";
const char __ada_exc_status_error[]     = "IO_EXCEPTIONS.STATUS_ERROR";
const char __ada_exc_mode_error[]       = "IO_EXCEPTIONS.MODE_ERROR";
const char __ada_exc_name_error[]       = "IO_EXCEPTIONS.NAME_ERROR";
const char __ada_exc_use_error[]        = "IO_EXCEPTIONS.USE_ERROR";
const char __ada_exc_device_error[]     = "IO_EXCEPTIONS.DEVICE_ERROR";
const char __ada_exc_end_error[]        = "IO_EXCEPTIONS.END_ERROR";
const char __ada_exc_data_error[]       = "IO_EXCEPTIONS.DATA_ERROR";
const char __ada_exc_layout_error[]     = "IO_EXCEPTIONS.LAYOUT_ERROR";
const char __ada_exc_time_error[]       = "CALENDAR.TIME_ERROR";

void __ada_raise(const void *exc_tag) {
    __pir_unwind_resume((void *)exc_tag);
    abort();
}

void __ada_unhandled_exception(const void *exc_tag) {
    const char *name = exc_tag ? (const char *)exc_tag : "UNKNOWN_EXCEPTION";
    fprintf(stderr, "raised %s\n", name);
    fflush(stderr);
    exit(1);
}

/* =========================================================================
 * Text_IO Runtime Support
 * ========================================================================= */

void __ada_put_char(int8_t c) {
    putchar((unsigned char)c);
}

void __ada_put_str(const char *ptr, int32_t len) {
    if (len > 0 && ptr != NULL) {
        fwrite(ptr, 1u, (size_t)len, stdout);
    }
}

void __ada_put_line(const char *ptr, int32_t len) {
    if (len > 0 && ptr != NULL) {
        fwrite(ptr, 1u, (size_t)len, stdout);
    }
    putchar('\n');
}

void __ada_new_line(int32_t spacing) {
    for (int32_t i = 0; i < spacing; ++i) {
        putchar('\n');
    }
}

void __ada_put_i64(int64_t val, int32_t width, int32_t base) {
    char buf[128];
    if (base == 10 || base < 2 || base > 16) {
        snprintf(buf, sizeof(buf), "%" PRId64, val);
    } else {
        /* Ada based literal format: BASE#DIGITS# */
        static const char digits[] = "0123456789ABCDEF";
        char rev[96];
        int neg = 0;
        uint64_t u = (uint64_t)val;
        if (val < 0) {
            neg = 1;
            u = (uint64_t)(-(val + 1)) + 1u;
        }
        int rlen = 0;
        if (u == 0) {
            rev[rlen++] = '0';
        } else {
            while (u > 0) {
                rev[rlen++] = digits[u % (uint64_t)base];
                u /= (uint64_t)base;
            }
        }
        int pos = snprintf(buf, sizeof(buf), "%s%d#", neg ? "-" : "", (int)base);
        for (int i = rlen - 1; i >= 0 && pos < (int)sizeof(buf) - 2; --i) {
            buf[pos++] = rev[i];
        }
        buf[pos++] = '#';
        buf[pos] = '\0';
    }
    int len = (int)strlen(buf);
    for (int i = len; i < (int)width; ++i) {
        putchar(' ');
    }
    fputs(buf, stdout);
}

void __ada_put_f64(double val, int32_t fore, int32_t aft, int32_t exp_digits) {
    if (aft < 1) aft = 1;
    char buf[128];
    if (exp_digits <= 0) {
        snprintf(buf, sizeof(buf), "%.*f", (int)aft, val);
    } else {
        snprintf(buf, sizeof(buf), "%.*E", (int)aft, val);
    }
    /* Pad fore (characters before '.') */
    const char *dot = strchr(buf, '.');
    int before_dot = dot ? (int)(dot - buf) : (int)strlen(buf);
    for (int i = before_dot; i < (int)fore; ++i) {
        putchar(' ');
    }
    fputs(buf, stdout);
}

/* =========================================================================
 * Calendar Runtime Support (Monotonic clock in nanoseconds)
 * ========================================================================= */

int64_t __ada_clock_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* =========================================================================
 * Exponentiation & Allocation Helpers
 * ========================================================================= */

int64_t __ada_ipow_i64(int64_t base, int32_t exp, int32_t checked) {
    if (exp < 0) {
        __ada_raise(__ada_exc_constraint_error);
    }
    int64_t res = 1;
    int64_t b = base;
    int32_t e = exp;
    while (e > 0) {
        if (e & 1) {
            if (checked) {
                __int128 prod = (__int128)res * (__int128)b;
                if (prod < INT64_MIN || prod > INT64_MAX) {
                    __ada_raise(__ada_exc_constraint_error);
                }
                res = (int64_t)prod;
            } else {
                res *= b;
            }
        }
        e >>= 1;
        if (e > 0) {
            if (checked) {
                __int128 prod = (__int128)b * (__int128)b;
                if (prod < INT64_MIN || prod > INT64_MAX) {
                    __ada_raise(__ada_exc_constraint_error);
                }
                b = (int64_t)prod;
            } else {
                b *= b;
            }
        }
    }
    return res;
}

int32_t __ada_ipow_i32(int32_t base, int32_t exp, int32_t checked) {
    if (exp < 0) {
        __ada_raise(__ada_exc_constraint_error);
    }
    int64_t r = __ada_ipow_i64((int64_t)base, exp, checked);
    if (checked && (r < INT32_MIN || r > INT32_MAX)) {
        __ada_raise(__ada_exc_constraint_error);
    }
    return (int32_t)r;
}

double __ada_fpow_f64(double base, int32_t exp) {
    double res = 1.0;
    double b = base;
    int64_t e = (int64_t)exp;
    int neg = 0;
    if (e < 0) {
        neg = 1;
        e = -e;
    }
    while (e > 0) {
        if (e & 1) res *= b;
        e >>= 1;
        if (e > 0) b *= b;
    }
    return neg ? (1.0 / res) : res;
}

float __ada_fpow_f32(float base, int32_t exp) {
    return (float)__ada_fpow_f64((double)base, exp);
}

void *__ada_alloc(int64_t bytes) {
    if (bytes <= 0) bytes = 1;
    void *p = calloc(1u, (size_t)bytes);
    if (!p) {
        __ada_raise(__ada_exc_storage_error);
    }
    return p;
}

void __ada_free(void *p) {
    free(p);
}

/* =========================================================================
 * Ada Secondary Stack (for unconstrained / dynamic array function returns
 * and `&` concatenation temporaries).
 *
 * Uses a linked list of pointer-stable chunks with cached reuse:
 *   - O(1) mark, O(1) bump-pointer alloc, O(1) release (zero malloc/free
 *     churn in loops)
 *   - Pointer stability: growing allocates a new linked chunk without
 *     moving previously returned arrays in earlier chunks
 *   - Exception safe: restoring a saved mark reclaims all unwound frames
 * ========================================================================= */

typedef struct AdaSSChunk {
    struct AdaSSChunk *prev;
    struct AdaSSChunk *next;
    uint64_t           base_offset;
    size_t             cap;
    size_t             used;
    uint8_t            data[];
} AdaSSChunk;

static AdaSSChunk *g_ada_ss_cur = NULL;

static AdaSSChunk *ada_ss_new_chunk(size_t cap, uint64_t base_offset) {
    if (cap < 65536u) cap = 65536u;
    AdaSSChunk *c = (AdaSSChunk *)malloc(sizeof(AdaSSChunk) + cap);
    if (!c) {
        __ada_raise(__ada_exc_storage_error);
    }
    c->prev        = NULL;
    c->next        = NULL;
    c->base_offset = base_offset;
    c->cap         = cap;
    c->used        = 0u;
    return c;
}

uint64_t __ada_ss_mark(void) {
    if (!g_ada_ss_cur) return 0u;
    return g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->used;
}

void *__ada_ss_alloc(int64_t bytes, int64_t align) {
    size_t sz = (bytes > 0) ? (size_t)bytes : 1u;
    size_t al = (align > 1) ? (size_t)align : 1u;
    if ((al & (al - 1u)) != 0u) al = 16u;
    if (al < 8u) al = 8u;

    if (!g_ada_ss_cur) {
        size_t init_cap = (sz + al > 65536u) ? (sz + al) : 65536u;
        g_ada_ss_cur = ada_ss_new_chunk(init_cap, 0u);
    }

    size_t aligned = (g_ada_ss_cur->used + (al - 1u)) & ~(al - 1u);
    if (aligned + sz <= g_ada_ss_cur->cap) {
        void *p = &g_ada_ss_cur->data[aligned];
        g_ada_ss_cur->used = aligned + sz;
        return p;
    }

    /* Fast reuse of cached next chunk if large enough. */
    if (g_ada_ss_cur->next && g_ada_ss_cur->next->cap >= sz) {
        g_ada_ss_cur = g_ada_ss_cur->next;
        g_ada_ss_cur->used = sz;
        return g_ada_ss_cur->data;
    }

    /* Free any too-small cached tail chunks above current top. */
    AdaSSChunk *tail = g_ada_ss_cur->next;
    while (tail) {
        AdaSSChunk *nx = tail->next;
        free(tail);
        tail = nx;
    }
    g_ada_ss_cur->next = NULL;

    size_t next_cap = g_ada_ss_cur->cap * 2u;
    if (next_cap < sz) next_cap = sz;
    uint64_t next_base = g_ada_ss_cur->base_offset + (uint64_t)g_ada_ss_cur->cap;
    AdaSSChunk *nc = ada_ss_new_chunk(next_cap, next_base);
    nc->prev = g_ada_ss_cur;
    g_ada_ss_cur->next = nc;
    g_ada_ss_cur = nc;
    g_ada_ss_cur->used = sz;
    return g_ada_ss_cur->data;
}

void __ada_ss_release(uint64_t mark) {
    if (!g_ada_ss_cur) return;
    while (g_ada_ss_cur->prev != NULL && g_ada_ss_cur->base_offset > mark) {
        g_ada_ss_cur->used = 0u;
        g_ada_ss_cur = g_ada_ss_cur->prev;
    }
    if (mark >= g_ada_ss_cur->base_offset) {
        size_t off = (size_t)(mark - g_ada_ss_cur->base_offset);
        g_ada_ss_cur->used = (off <= g_ada_ss_cur->cap) ? off : g_ada_ss_cur->cap;
    } else {
        g_ada_ss_cur->used = 0u;
    }
}
