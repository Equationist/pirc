/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Freestanding C99 runtime helpers for bare-metal PowerPC 32-bit Big-Endian
 * (powerpc-unknown-elf on QEMU bamboo):
 *   - 64-bit div/rem (__divdi3, __udivdi3, __moddi3, __umoddi3)
 *   - 128-bit div/rem (__divti3, __udivti3, __modti3, __umodti3) (big-endian U128)
 *   - 64-bit software atomics (__pir_atomic_*_8)
 *   - Shadow-stack exception unwinder (__pir_unwind_resume, __pir_exc_head)
 *   - Minimal freestanding libc (memcpy, memmove, memset, memcmp, strlen,
 *     bump malloc/calloc/realloc/free, putchar, puts, printf, exit, abort)
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern void _pir_ppc32_exit(int code) __attribute__((noreturn));
extern void _pir_ppc32_putc(int ch);
extern void __pir_longjmp(void *jb, int val) __attribute__((noreturn));

/* =========================================================================
 * 64-bit Division / Remainder (SysV PPC32 ABI: a in r3:r4 (hi:lo), b in r5:r6)
 * ========================================================================= */

static void u64_divmod(uint64_t a, uint64_t b, uint64_t *q_out, uint64_t *r_out) {
    uint64_t q = 0u, r = 0u;
    for (int bit = 63; bit >= 0; --bit) {
        r = (r << 1u) | ((a >> (unsigned)bit) & 1u);
        if (r >= b) {
            r -= b;
            q |= (uint64_t)1u << (unsigned)bit;
        }
    }
    *q_out = q;
    *r_out = r;
}

uint64_t __udivdi3(uint64_t a, uint64_t b);
uint64_t __umoddi3(uint64_t a, uint64_t b);
int64_t  __divdi3(int64_t a, int64_t b);
int64_t  __moddi3(int64_t a, int64_t b);

uint64_t __udivdi3(uint64_t a, uint64_t b) {
    uint64_t q = 0u, r = 0u;
    u64_divmod(a, b, &q, &r);
    return q;
}

uint64_t __umoddi3(uint64_t a, uint64_t b) {
    uint64_t q = 0u, r = 0u;
    u64_divmod(a, b, &q, &r);
    return r;
}

int64_t __divdi3(int64_t a, int64_t b) {
    int neg = 0;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (a < 0) { neg = !neg; ua = ~ua + 1u; }
    if (b < 0) { neg = !neg; ub = ~ub + 1u; }
    uint64_t q = 0u, r = 0u;
    u64_divmod(ua, ub, &q, &r);
    return neg ? (int64_t)(~q + 1u) : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b) {
    int neg = (a < 0);
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (a < 0) ua = ~ua + 1u;
    if (b < 0) ub = ~ub + 1u;
    uint64_t q = 0u, r = 0u;
    u64_divmod(ua, ub, &q, &r);
    return neg ? (int64_t)(~r + 1u) : (int64_t)r;
}

/* =========================================================================
 * 128-bit Division / Remainder (Big-endian U128 { hi, lo } in sret *out)
 * ========================================================================= */

typedef struct { uint64_t hi, lo; } U128;

static void u128_neg(U128 *v) {
    uint64_t nlo = ~v->lo + 1u;
    uint64_t cy  = (nlo == 0u) ? 1u : 0u;
    v->lo = nlo;
    v->hi = ~v->hi + cy;
}

static void u128_divmod(U128 a, U128 b, U128 *q_out, U128 *r_out) {
    U128 q = {0u, 0u}, r = {0u, 0u};
    for (int bit = 127; bit >= 0; --bit) {
        r.hi = (r.hi << 1u) | (r.lo >> 63u);
        r.lo <<= 1u;
        uint64_t ab = (bit >= 64) ? ((a.hi >> (unsigned)(bit - 64)) & 1u)
                                  : ((a.lo >> (unsigned)bit) & 1u);
        r.lo |= ab;
        if (r.hi > b.hi || (r.hi == b.hi && r.lo >= b.lo)) {
            uint64_t bw = (r.lo < b.lo) ? 1u : 0u;
            r.lo -= b.lo;
            r.hi = r.hi - b.hi - bw;
            if (bit >= 64) q.hi |= (uint64_t)1u << (unsigned)(bit - 64);
            else           q.lo |= (uint64_t)1u << (unsigned)bit;
        }
    }
    *q_out = q;
    *r_out = r;
}

static void ti3_common(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi,
                       int is_signed, int want_rem) {
    U128 a = {ahi, alo}, b = {bhi, blo};
    int na = 0, nb = 0;
    if (is_signed) {
        if (a.hi >> 63u) { na = 1; u128_neg(&a); }
        if (b.hi >> 63u) { nb = 1; u128_neg(&b); }
    }
    U128 q, r;
    u128_divmod(a, b, &q, &r);
    U128 res = want_rem ? r : q;
    if (is_signed && (want_rem ? na : (na != nb))) u128_neg(&res);
    *out = res;
}

void __divti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi);
void __udivti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi);
void __modti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi);
void __umodti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi);

void __divti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi)  { ti3_common(out, alo, ahi, blo, bhi, 1, 0); }
void __udivti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi) { ti3_common(out, alo, ahi, blo, bhi, 0, 0); }
void __modti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi)  { ti3_common(out, alo, ahi, blo, bhi, 1, 1); }
void __umodti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi) { ti3_common(out, alo, ahi, blo, bhi, 0, 1); }

/* =========================================================================
 * 64-bit Software Atomics (single-core bare-metal)
 * ========================================================================= */

void    __pir_atomic_load_8(const volatile uint64_t *p, uint64_t *out);
void    __pir_atomic_store_8(volatile uint64_t *p, uint32_t lo, uint32_t hi);
void    __pir_atomic_rmw_8(volatile uint64_t *p, uint64_t *out, uint32_t op, uint32_t lo, uint32_t hi);
uint8_t __pir_atomic_cmpxchg_8(volatile uint64_t *p, uint64_t *out,
                               uint32_t elo, uint32_t ehi, uint32_t nlo, uint32_t nhi);

void __pir_atomic_load_8(const volatile uint64_t *p, uint64_t *out) {
    *out = *p;
}

void __pir_atomic_store_8(volatile uint64_t *p, uint32_t lo, uint32_t hi) {
    *p = (uint64_t)lo | ((uint64_t)hi << 32u);
}

void __pir_atomic_rmw_8(volatile uint64_t *p, uint64_t *out, uint32_t op, uint32_t lo, uint32_t hi) {
    uint64_t old = *p;
    uint64_t rhs = (uint64_t)lo | ((uint64_t)hi << 32u);
    uint64_t nv  = old;
    if (op == 0u)      nv = old + rhs;
    else if (op == 1u) nv = old - rhs;
    else if (op == 2u) nv = old & rhs;
    else if (op == 3u) nv = old | rhs;
    else if (op == 4u) nv = old ^ rhs;
    else if (op == 5u) nv = rhs;
    *out = old;
    *p   = nv;
}

uint8_t __pir_atomic_cmpxchg_8(volatile uint64_t *p, uint64_t *out,
                               uint32_t elo, uint32_t ehi, uint32_t nlo, uint32_t nhi) {
    uint64_t old = *p;
    uint64_t exp = (uint64_t)elo | ((uint64_t)ehi << 32u);
    uint64_t des = (uint64_t)nlo | ((uint64_t)nhi << 32u);
    *out = old;
    if (old == exp) {
        *p = des;
        return 1u;
    }
    return 0u;
}

/* =========================================================================
 * Shadow-Stack Exception Unwinder
 * ========================================================================= */

typedef struct PirExcFrame {
    struct PirExcFrame *prev;
    void               *exn;
    uint8_t             jb[384];
} PirExcFrame;

__attribute__((weak)) __thread PirExcFrame *__pir_exc_head = NULL;

void __pir_unwind_resume(void *exn) __attribute__((noreturn));
void __pir_unwind_resume(void *exn) {
    PirExcFrame *f = __pir_exc_head;
    if (!f) {
        _pir_ppc32_exit(132);
    }
    __pir_exc_head = f->prev;
    f->exn = exn;
    __pir_longjmp(f->jb, 1);
}

/* =========================================================================
 * Minimal Freestanding C Library Shims (for PIR execution tests)
 * ========================================================================= */

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0u; i < n; ++i) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0u; i < n; ++i) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0u; --i) d[i - 1u] = s[i - 1u];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0u; i < n; ++i) d[i] = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0u; i < n; ++i) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0u;
    while (s[n]) n++;
    return n;
}

extern uint8_t __heap_start[];
extern uint8_t __heap_end[];
static uint8_t *heap_cur = NULL;

void *malloc(size_t sz);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t sz);
void  free(void *p);

void *malloc(size_t sz) {
    if (!heap_cur) heap_cur = __heap_start;
    if (sz == 0u) sz = 1u;
    size_t total = ((sz + 15u) & ~(size_t)15u) + 16u;
    if ((size_t)(__heap_end - heap_cur) < total) return NULL;
    size_t *hdr = (size_t *)heap_cur;
    *hdr = sz;
    void *res = heap_cur + 16u;
    heap_cur += total;
    return res;
}

void *calloc(size_t n, size_t sz) {
    size_t tot = n * sz;
    void *p = malloc(tot);
    if (p) memset(p, 0, tot);
    return p;
}

void *realloc(void *p, size_t sz) {
    if (!p) return malloc(sz);
    size_t old_sz = *((size_t *)((uint8_t *)p - 16u));
    void *np = malloc(sz);
    if (!np) return NULL;
    memcpy(np, p, old_sz < sz ? old_sz : sz);
    return np;
}

void free(void *p) {
    (void)p;
}

int  putchar(int c);
int  puts(const char *s);
int  printf(const char *fmt, ...);
void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int putchar(int c) {
    _pir_ppc32_putc(c & 0xFF);
    return c & 0xFF;
}

int puts(const char *s) {
    int n = 0;
    while (*s) { _pir_ppc32_putc((unsigned char)*s++); n++; }
    _pir_ppc32_putc('\n');
    return n + 1;
}

static int print_uint(uint64_t v, unsigned base, int upper) {
    char buf[32];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0u) { _pir_ppc32_putc('0'); return 1; }
    while (v > 0u) {
        uint64_t q = 0u, r = 0u;
        u64_divmod(v, base, &q, &r);
        buf[n++] = digs[r];
        v = q;
    }
    for (int i = n - 1; i >= 0; --i) _pir_ppc32_putc((unsigned char)buf[i]);
    return n;
}

static int print_double(double v, int prec) {
    int n = 0;
    if (v != v) {
        const char *s = "nan";
        while (*s) { _pir_ppc32_putc((unsigned char)*s++); n++; }
        return n;
    }
    union { double d; uint64_t u; } bits;
    bits.d = v;
    if (bits.u >> 63u) {
        _pir_ppc32_putc('-');
        n++;
        v = -v;
    }
    if (v > 4e9) {
        const char *s = "inf";
        while (*s) { _pir_ppc32_putc((unsigned char)*s++); n++; }
        return n;
    }
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    uint32_t iscale = 1u;
    for (int i = 0; i < prec; ++i) iscale *= 10u;
    uint32_t ipart = (uint32_t)v;
    double frac = v - (double)ipart;
    uint32_t fpart = (uint32_t)(frac * (double)iscale + 0.5);
    if (fpart >= iscale) {
        fpart -= iscale;
        ipart++;
    }
    n += print_uint((uint64_t)ipart, 10u, 0);
    if (prec > 0) {
        _pir_ppc32_putc('.');
        n++;
        char fbuf[16];
        for (int i = prec - 1; i >= 0; --i) {
            fbuf[i] = (char)('0' + (fpart % 10u));
            fpart /= 10u;
        }
        for (int i = 0; i < prec; ++i) {
            _pir_ppc32_putc((unsigned char)fbuf[i]);
            n++;
        }
    }
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int total = 0;
    for (const char *s = fmt; *s; ++s) {
        if (*s != '%') { _pir_ppc32_putc((unsigned char)*s); total++; continue; }
        s++;
        if (*s == '%') { _pir_ppc32_putc('%'); total++; continue; }
        int is_ll = 0;
        int prec  = -1;
        while (*s == '-' || *s == '+' || *s == ' ' || *s == '0' || *s == '#') s++;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') {
            s++;
            prec = 0;
            while (*s >= '0' && *s <= '9') { prec = prec * 10 + (*s - '0'); s++; }
        }
        if (*s == 'l') { s++; if (*s == 'l') { is_ll = 1; s++; } }
        if (*s == 'd' || *s == 'i') {
            int64_t v = is_ll ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) { _pir_ppc32_putc('-'); total++; total += print_uint(~(uint64_t)v + 1u, 10u, 0); }
            else       { total += print_uint((uint64_t)v, 10u, 0); }
        } else if (*s == 'u') {
            uint64_t v = is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            total += print_uint(v, 10u, 0);
        } else if (*s == 'x' || *s == 'X') {
            uint64_t v = is_ll ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t);
            total += print_uint(v, 16u, *s == 'X');
        } else if (*s == 'f' || *s == 'F') {
            double v = va_arg(ap, double);
            total += print_double(v, prec);
        } else if (*s == 'c') {
            _pir_ppc32_putc(va_arg(ap, int) & 0xFF);
            total++;
        } else if (*s == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            while (*str) { _pir_ppc32_putc((unsigned char)*str++); total++; }
        }
    }
    va_end(ap);
    return total;
}

void exit(int code) {
    _pir_ppc32_exit(code & 0xFF);
}

void abort(void) {
    _pir_ppc32_exit(132);
}
