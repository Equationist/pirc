/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Freestanding C99 runtime for bare-metal AVR (ATmega1280, QEMU -M mega):
 *   - 16/32/64/128-bit integer div/rem (__*hi3, __*si3, __*di3, __*ti3)
 *   - Pure-integer IEEE-754 single & double soft-float (__addsf3..__pir_fcmp_df)
 *   - 32-bit & 64-bit interrupt-masked atomics (__pir_atomic_*_{4,8})
 *   - Shadow-stack exception unwinder (__pir_unwind_resume, __pir_exc_head)
 *   - Minimal freestanding libc + printf (pure integer, zero libgcc deps)
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern void _pir_avr_exit(int16_t code) __attribute__((noreturn));
extern void _pir_avr_putc(int16_t ch);
extern void __pir_longjmp(void *jb, int16_t val) __attribute__((noreturn));

static inline uint8_t avr_irq_save(void) {
    uint8_t sreg;
    __asm__ volatile("in %0, 0x3f\n\tcli" : "=r"(sreg) : : "memory");
    return sreg;
}

static inline void avr_irq_restore(uint8_t sreg) {
    __asm__ volatile("out 0x3f, %0" : : "r"(sreg) : "memory");
}

/* =========================================================================
 * 1. Integer Division / Remainder (16-bit, 32-bit, 64-bit, 128-bit)
 * ========================================================================= */

static void u16_divmod(uint16_t a, uint16_t b, uint16_t *qo, uint16_t *ro) {
    uint16_t q = 0u, r = 0u;
    for (int8_t bit = 15; bit >= 0; --bit) {
        r = (uint16_t)((r << 1u) | ((a >> (uint8_t)bit) & 1u));
        if (r >= b) {
            r = (uint16_t)(r - b);
            q = (uint16_t)(q | (uint16_t)(1u << (uint8_t)bit));
        }
    }
    *qo = q; *ro = r;
}

uint16_t __udivhi3(uint16_t a, uint16_t b);
uint16_t __umodhi3(uint16_t a, uint16_t b);
int16_t  __divhi3(int16_t a, int16_t b);
int16_t  __modhi3(int16_t a, int16_t b);

uint16_t __udivhi3(uint16_t a, uint16_t b) { uint16_t q, r; u16_divmod(a, b, &q, &r); return q; }
uint16_t __umodhi3(uint16_t a, uint16_t b) { uint16_t q, r; u16_divmod(a, b, &q, &r); return r; }

int16_t __divhi3(int16_t a, int16_t b) {
    uint8_t neg = 0u;
    uint16_t ua = (uint16_t)a, ub = (uint16_t)b;
    if (a < 0) { neg ^= 1u; ua = (uint16_t)(0u - ua); }
    if (b < 0) { neg ^= 1u; ub = (uint16_t)(0u - ub); }
    uint16_t q, r;
    u16_divmod(ua, ub, &q, &r);
    return neg ? (int16_t)(0u - q) : (int16_t)q;
}

int16_t __modhi3(int16_t a, int16_t b) {
    uint8_t neg = (a < 0) ? 1u : 0u;
    uint16_t ua = (uint16_t)a, ub = (uint16_t)b;
    if (a < 0) ua = (uint16_t)(0u - ua);
    if (b < 0) ub = (uint16_t)(0u - ub);
    uint16_t q, r;
    u16_divmod(ua, ub, &q, &r);
    return neg ? (int16_t)(0u - r) : (int16_t)r;
}

static void u32_divmod(uint32_t a, uint32_t b, uint32_t *qo, uint32_t *ro) {
    uint32_t q = 0u, r = 0u;
    for (int8_t bit = 31; bit >= 0; --bit) {
        r = (r << 1u) | ((a >> (uint8_t)bit) & 1u);
        if (r >= b) {
            r -= b;
            q |= ((uint32_t)1u << (uint8_t)bit);
        }
    }
    *qo = q; *ro = r;
}

uint32_t __udivsi3(uint32_t a, uint32_t b);
uint32_t __umodsi3(uint32_t a, uint32_t b);
int32_t  __divsi3(int32_t a, int32_t b);
int32_t  __modsi3(int32_t a, int32_t b);

uint32_t __udivsi3(uint32_t a, uint32_t b) { uint32_t q, r; u32_divmod(a, b, &q, &r); return q; }
uint32_t __umodsi3(uint32_t a, uint32_t b) { uint32_t q, r; u32_divmod(a, b, &q, &r); return r; }

int32_t __divsi3(int32_t a, int32_t b) {
    uint8_t neg = 0u;
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    if (a < 0) { neg ^= 1u; ua = 0u - ua; }
    if (b < 0) { neg ^= 1u; ub = 0u - ub; }
    uint32_t q, r;
    u32_divmod(ua, ub, &q, &r);
    return neg ? (int32_t)(0u - q) : (int32_t)q;
}

int32_t __modsi3(int32_t a, int32_t b) {
    uint8_t neg = (a < 0) ? 1u : 0u;
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    if (a < 0) ua = 0u - ua;
    if (b < 0) ub = 0u - ub;
    uint32_t q, r;
    u32_divmod(ua, ub, &q, &r);
    return neg ? (int32_t)(0u - r) : (int32_t)r;
}

static void u64_divmod(uint64_t a, uint64_t b, uint64_t *qo, uint64_t *ro) {
    uint64_t q = 0u, r = 0u;
    for (int8_t bit = 63; bit >= 0; --bit) {
        r = (r << 1u) | ((a >> (uint8_t)bit) & 1u);
        if (r >= b) {
            r -= b;
            q |= ((uint64_t)1u << (uint8_t)bit);
        }
    }
    *qo = q; *ro = r;
}

uint64_t __udivdi3(uint64_t a, uint64_t b);
uint64_t __umoddi3(uint64_t a, uint64_t b);
int64_t  __divdi3(int64_t a, int64_t b);
int64_t  __moddi3(int64_t a, int64_t b);

uint64_t __udivdi3(uint64_t a, uint64_t b) { uint64_t q, r; u64_divmod(a, b, &q, &r); return q; }
uint64_t __umoddi3(uint64_t a, uint64_t b) { uint64_t q, r; u64_divmod(a, b, &q, &r); return r; }

int64_t __divdi3(int64_t a, int64_t b) {
    uint8_t neg = 0u;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (a < 0) { neg ^= 1u; ua = 0u - ua; }
    if (b < 0) { neg ^= 1u; ub = 0u - ub; }
    uint64_t q, r;
    u64_divmod(ua, ub, &q, &r);
    return neg ? (int64_t)(0u - q) : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b) {
    uint8_t neg = (a < 0) ? 1u : 0u;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (a < 0) ua = 0u - ua;
    if (b < 0) ub = 0u - ub;
    uint64_t q, r;
    u64_divmod(ua, ub, &q, &r);
    return neg ? (int64_t)(0u - r) : (int64_t)r;
}

/* 128-bit little-endian division/remainder */
typedef struct { uint64_t lo, hi; } U128;

static void u128_neg(U128 *v) {
    uint64_t nlo = ~v->lo + 1u;
    uint64_t cy  = (nlo == 0u) ? 1u : 0u;
    v->lo = nlo;
    v->hi = ~v->hi + cy;
}

static void u128_divmod(U128 a, U128 b, U128 *qo, U128 *ro) {
    U128 q = {0u, 0u}, r = {0u, 0u};
    for (int16_t bit = 127; bit >= 0; --bit) {
        r.hi = (r.hi << 1u) | (r.lo >> 63u);
        r.lo <<= 1u;
        uint64_t ab = (bit >= 64) ? ((a.hi >> (uint8_t)(bit - 64)) & 1u)
                                  : ((a.lo >> (uint8_t)bit) & 1u);
        r.lo |= ab;
        if (r.hi > b.hi || (r.hi == b.hi && r.lo >= b.lo)) {
            uint64_t bw = (r.lo < b.lo) ? 1u : 0u;
            r.lo -= b.lo;
            r.hi = r.hi - b.hi - bw;
            if (bit >= 64) q.hi |= ((uint64_t)1u << (uint8_t)(bit - 64));
            else           q.lo |= ((uint64_t)1u << (uint8_t)bit);
        }
    }
    *qo = q; *ro = r;
}

static void ti3_common(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi,
                       uint8_t is_signed, uint8_t want_rem) {
    U128 a = {alo, ahi}, b = {blo, bhi};
    uint8_t na = 0u, nb = 0u;
    if (is_signed) {
        if (a.hi >> 63u) { na = 1u; u128_neg(&a); }
        if (b.hi >> 63u) { nb = 1u; u128_neg(&b); }
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

void __divti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi)  { ti3_common(out, alo, ahi, blo, bhi, 1u, 0u); }
void __udivti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi) { ti3_common(out, alo, ahi, blo, bhi, 0u, 0u); }
void __modti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi)  { ti3_common(out, alo, ahi, blo, bhi, 1u, 1u); }
void __umodti3(U128 *out, uint64_t alo, uint64_t ahi, uint64_t blo, uint64_t bhi) { ti3_common(out, alo, ahi, blo, bhi, 0u, 1u); }

/* Clang AVR libgcc helper shims (in case Clang emits them for C 32/64-bit ops in this file) */
uint32_t __mulsi3(uint32_t a, uint32_t b);
uint64_t __muldi3(uint64_t a, uint64_t b);

uint32_t __mulsi3(uint32_t a, uint32_t b) {
    uint32_t res = 0u;
    while (b != 0u) {
        if (b & 1u) res += a;
        a <<= 1u;
        b >>= 1u;
    }
    return res;
}

uint64_t __muldi3(uint64_t a, uint64_t b) {
    uint64_t res = 0u;
    while (b != 0u) {
        if (b & 1u) res += a;
        a <<= 1u;
        b >>= 1u;
    }
    return res;
}

/* =========================================================================
 * 2. Pure-Integer IEEE-754 Soft-Float (binary32 & binary64, Round-to-Nearest-Even)
 * ========================================================================= */

static uint8_t clz32(uint32_t x) {
    uint8_t n = 0u;
    if (x == 0u) return 32u;
    while ((x & UINT32_C(0x80000000)) == 0u) { x <<= 1u; n++; }
    return n;
}

static uint8_t clz64(uint64_t x) {
    uint8_t n = 0u;
    if (x == 0u) return 64u;
    while ((x & (UINT64_C(1) << 63u)) == 0u) { x <<= 1u; n++; }
    return n;
}

static uint32_t rshift_sticky32(uint32_t x, uint32_t sh) {
    if (sh == 0u) return x;
    if (sh >= 32u) return x ? 1u : 0u;
    uint32_t lost = x & ((UINT32_C(1) << sh) - 1u);
    return (x >> sh) | (lost ? 1u : 0u);
}

static uint64_t rshift_sticky64(uint64_t x, uint32_t sh) {
    if (sh == 0u) return x;
    if (sh >= 64u) return x ? 1u : 0u;
    uint64_t lost = x & ((UINT64_C(1) << sh) - 1u);
    return (x >> sh) | (lost ? 1u : 0u);
}

/* Pack binary32 from sign, unbiased-ish exp (with mantissa normalized so bit 26 is hidden 1,
 * i.e. 3 GRS bits at bits 2..0). */
static uint32_t pack_sf(uint32_t sign, int32_t exp, uint32_t sig) {
    if (sig == 0u) return sign << 31u;
    while (sig >= (UINT32_C(1) << 27u)) {
        uint32_t st = sig & 1u;
        sig = (sig >> 1u) | st;
        exp++;
    }
    while (sig < (UINT32_C(1) << 26u)) {
        sig <<= 1u;
        exp--;
    }
    if (exp <= 0) {
        uint32_t sh = (uint32_t)(1 - exp);
        sig = rshift_sticky32(sig, sh);
        exp = 0;
    }
    uint32_t grs = sig & 7u;
    uint32_t mant = sig >> 3u;
    if (grs > 4u || (grs == 4u && (mant & 1u) != 0u)) {
        mant++;
        if (exp == 0 && mant == (UINT32_C(1) << 23u)) {
            exp = 1;
        } else if (mant >= (UINT32_C(1) << 24u)) {
            mant >>= 1u;
            exp++;
        }
    }
    if (exp >= 255) {
        return (sign << 31u) | UINT32_C(0x7F800000);
    }
    return (sign << 31u) | ((uint32_t)exp << 23u) | (mant & UINT32_C(0x007FFFFF));
}

/* Pack binary64 from sign, biased exp, 56-bit sig (bit 55 = hidden 1, bits 2..0 = GRS). */
static uint64_t pack_df(uint64_t sign, int32_t exp, uint64_t sig) {
    if (sig == 0u) return sign << 63u;
    while (sig >= (UINT64_C(1) << 56u)) {
        uint64_t st = sig & 1u;
        sig = (sig >> 1u) | st;
        exp++;
    }
    while (sig < (UINT64_C(1) << 55u)) {
        sig <<= 1u;
        exp--;
    }
    if (exp <= 0) {
        uint32_t sh = (uint32_t)(1 - exp);
        sig = rshift_sticky64(sig, sh);
        exp = 0;
    }
    uint64_t grs = sig & 7u;
    uint64_t mant = sig >> 3u;
    if (grs > 4u || (grs == 4u && (mant & 1u) != 0u)) {
        mant++;
        if (exp == 0 && mant == (UINT64_C(1) << 52u)) {
            exp = 1;
        } else if (mant >= (UINT64_C(1) << 53u)) {
            mant >>= 1u;
            exp++;
        }
    }
    if (exp >= 2047) {
        return (sign << 63u) | UINT64_C(0x7FF0000000000000);
    }
    return (sign << 63u) | ((uint64_t)exp << 52u) | (mant & UINT64_C(0x000FFFFFFFFFFFFF));
}

/* --- binary32 arithmetic --- */
uint32_t __addsf3(uint32_t a, uint32_t b);
uint32_t __subsf3(uint32_t a, uint32_t b);
uint32_t __mulsf3(uint32_t a, uint32_t b);
uint32_t __divsf3(uint32_t a, uint32_t b);
int8_t   __pir_fcmp_sf(uint32_t a, uint32_t b);

uint32_t __addsf3(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31u, sb = b >> 31u;
    uint32_t ea = (a >> 23u) & 0xFFu, eb = (b >> 23u) & 0xFFu;
    uint32_t ma = a & UINT32_C(0x7FFFFF), mb = b & UINT32_C(0x7FFFFF);
    if (ea == 255u) {
        if (ma != 0u) return UINT32_C(0x7FC00000);
        if (eb == 255u && mb != 0u) return UINT32_C(0x7FC00000);
        if (eb == 255u && sa != sb) return UINT32_C(0x7FC00000);
        return a;
    }
    if (eb == 255u) return (mb != 0u) ? UINT32_C(0x7FC00000) : b;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint32_t sig_a = (ea ? (ma | UINT32_C(0x800000)) : ma) << 3u;
    uint32_t sig_b = (eb ? (mb | UINT32_C(0x800000)) : mb) << 3u;
    if (sig_a == 0u && sig_b == 0u) return (sa & sb) << 31u;
    if (exp_a < exp_b || (exp_a == exp_b && sig_a < sig_b)) {
        uint32_t ts = sa; sa = sb; sb = ts;
        int32_t te = exp_a; exp_a = exp_b; exp_b = te;
        uint32_t tg = sig_a; sig_a = sig_b; sig_b = tg;
    }
    sig_b = rshift_sticky32(sig_b, (uint32_t)(exp_a - exp_b));
    if (sa == sb) {
        return pack_sf(sa, exp_a, sig_a + sig_b);
    }
    uint32_t diff = sig_a - sig_b;
    if (diff == 0u) return 0u;
    return pack_sf(sa, exp_a, diff);
}

uint32_t __subsf3(uint32_t a, uint32_t b) {
    return __addsf3(a, b ^ UINT32_C(0x80000000));
}

uint32_t __mulsf3(uint32_t a, uint32_t b) {
    uint32_t sr = (a ^ b) >> 31u;
    uint32_t ea = (a >> 23u) & 0xFFu, eb = (b >> 23u) & 0xFFu;
    uint32_t ma = a & UINT32_C(0x7FFFFF), mb = b & UINT32_C(0x7FFFFF);
    if ((ea == 255u && ma != 0u) || (eb == 255u && mb != 0u)) return UINT32_C(0x7FC00000);
    if (ea == 255u || eb == 255u) {
        if ((ea == 0u && ma == 0u) || (eb == 0u && mb == 0u)) return UINT32_C(0x7FC00000);
        return (sr << 31u) | UINT32_C(0x7F800000);
    }
    if ((ea == 0u && ma == 0u) || (eb == 0u && mb == 0u)) return sr << 31u;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint32_t sig_a = ea ? (ma | UINT32_C(0x800000)) : ma;
    uint32_t sig_b = eb ? (mb | UINT32_C(0x800000)) : mb;
    while ((sig_a & UINT32_C(0x800000)) == 0u) { sig_a <<= 1u; exp_a--; }
    while ((sig_b & UINT32_C(0x800000)) == 0u) { sig_b <<= 1u; exp_b--; }
    uint64_t prod = __muldi3((uint64_t)sig_a, (uint64_t)sig_b);
    int32_t exp = exp_a + exp_b - 127;
    if (prod & (UINT64_C(1) << 47u)) {
        prod = rshift_sticky64(prod, 21u);
        exp++;
    } else {
        prod = rshift_sticky64(prod, 20u);
    }
    return pack_sf(sr, exp, (uint32_t)prod);
}

uint32_t __divsf3(uint32_t a, uint32_t b) {
    uint32_t sr = (a ^ b) >> 31u;
    uint32_t ea = (a >> 23u) & 0xFFu, eb = (b >> 23u) & 0xFFu;
    uint32_t ma = a & UINT32_C(0x7FFFFF), mb = b & UINT32_C(0x7FFFFF);
    if ((ea == 255u && ma != 0u) || (eb == 255u && mb != 0u)) return UINT32_C(0x7FC00000);
    if (ea == 255u) {
        if (eb == 255u) return UINT32_C(0x7FC00000);
        return (sr << 31u) | UINT32_C(0x7F800000);
    }
    if (eb == 255u) return sr << 31u;
    if (eb == 0u && mb == 0u) {
        if (ea == 0u && ma == 0u) return UINT32_C(0x7FC00000);
        return (sr << 31u) | UINT32_C(0x7F800000);
    }
    if (ea == 0u && ma == 0u) return sr << 31u;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint32_t sig_a = ea ? (ma | UINT32_C(0x800000)) : ma;
    uint32_t sig_b = eb ? (mb | UINT32_C(0x800000)) : mb;
    while ((sig_a & UINT32_C(0x800000)) == 0u) { sig_a <<= 1u; exp_a--; }
    while ((sig_b & UINT32_C(0x800000)) == 0u) { sig_b <<= 1u; exp_b--; }
    int32_t exp = exp_a - exp_b + 127;
    if (sig_a < sig_b) { sig_a <<= 1u; exp--; }
    uint32_t q = 0u, rem = sig_a;
    for (int8_t i = 26; i >= 0; --i) {
        if (rem >= sig_b) {
            rem -= sig_b;
            q |= (UINT32_C(1) << (uint8_t)i);
        }
        rem <<= 1u;
    }
    if (rem != 0u) q |= 1u;
    return pack_sf(sr, exp, q);
}

int8_t __pir_fcmp_sf(uint32_t a, uint32_t b) {
    uint32_t aa = a & UINT32_C(0x7FFFFFFF), ba = b & UINT32_C(0x7FFFFFFF);
    if (aa > UINT32_C(0x7F800000) || ba > UINT32_C(0x7F800000)) return 2;
    if (aa == 0u && ba == 0u) return 0;
    if (a == b) return 0;
    uint32_t sa = a >> 31u, sb = b >> 31u;
    if (sa != sb) return sa ? -1 : 1;
    if (sa) return (aa > ba) ? -1 : 1;
    return (aa < ba) ? -1 : 1;
}

/* --- binary64 arithmetic --- */
uint64_t __adddf3(uint64_t a, uint64_t b);
uint64_t __subdf3(uint64_t a, uint64_t b);
uint64_t __muldf3(uint64_t a, uint64_t b);
uint64_t __divdf3(uint64_t a, uint64_t b);
int8_t   __pir_fcmp_df(uint64_t a, uint64_t b);

uint64_t __adddf3(uint64_t a, uint64_t b) {
    uint64_t sa = a >> 63u, sb = b >> 63u;
    uint32_t ea = (uint32_t)((a >> 52u) & 0x7FFu), eb = (uint32_t)((b >> 52u) & 0x7FFu);
    uint64_t ma = a & UINT64_C(0x000FFFFFFFFFFFFF), mb = b & UINT64_C(0x000FFFFFFFFFFFFF);
    if (ea == 2047u) {
        if (ma != 0u) return UINT64_C(0x7FF8000000000000);
        if (eb == 2047u && mb != 0u) return UINT64_C(0x7FF8000000000000);
        if (eb == 2047u && sa != sb) return UINT64_C(0x7FF8000000000000);
        return a;
    }
    if (eb == 2047u) return (mb != 0u) ? UINT64_C(0x7FF8000000000000) : b;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint64_t sig_a = (ea ? (ma | (UINT64_C(1) << 52u)) : ma) << 3u;
    uint64_t sig_b = (eb ? (mb | (UINT64_C(1) << 52u)) : mb) << 3u;
    if (sig_a == 0u && sig_b == 0u) return (sa & sb) << 63u;
    if (exp_a < exp_b || (exp_a == exp_b && sig_a < sig_b)) {
        uint64_t ts = sa; sa = sb; sb = ts;
        int32_t te = exp_a; exp_a = exp_b; exp_b = te;
        uint64_t tg = sig_a; sig_a = sig_b; sig_b = tg;
    }
    sig_b = rshift_sticky64(sig_b, (uint32_t)(exp_a - exp_b));
    if (sa == sb) {
        return pack_df(sa, exp_a, sig_a + sig_b);
    }
    uint64_t diff = sig_a - sig_b;
    if (diff == 0u) return 0u;
    return pack_df(sa, exp_a, diff);
}

uint64_t __subdf3(uint64_t a, uint64_t b) {
    return __adddf3(a, b ^ (UINT64_C(1) << 63u));
}

/* 64x64 -> 128 unsigned multiply for binary64 __muldf3 */
static void u64_mul_wide(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi) {
    uint64_t a0 = a & UINT32_MAX, a1 = a >> 32u;
    uint64_t b0 = b & UINT32_MAX, b1 = b >> 32u;
    uint64_t p00 = __muldi3(a0, b0);
    uint64_t p01 = __muldi3(a0, b1);
    uint64_t p10 = __muldi3(a1, b0);
    uint64_t p11 = __muldi3(a1, b1);
    uint64_t mid = (p00 >> 32u) + (p01 & UINT32_MAX) + (p10 & UINT32_MAX);
    *lo = (p00 & UINT32_MAX) | (mid << 32u);
    *hi = p11 + (p01 >> 32u) + (p10 >> 32u) + (mid >> 32u);
}

uint64_t __muldf3(uint64_t a, uint64_t b) {
    uint64_t sr = (a ^ b) >> 63u;
    uint32_t ea = (uint32_t)((a >> 52u) & 0x7FFu), eb = (uint32_t)((b >> 52u) & 0x7FFu);
    uint64_t ma = a & UINT64_C(0x000FFFFFFFFFFFFF), mb = b & UINT64_C(0x000FFFFFFFFFFFFF);
    if ((ea == 2047u && ma != 0u) || (eb == 2047u && mb != 0u)) return UINT64_C(0x7FF8000000000000);
    if (ea == 2047u || eb == 2047u) {
        if ((ea == 0u && ma == 0u) || (eb == 0u && mb == 0u)) return UINT64_C(0x7FF8000000000000);
        return (sr << 63u) | UINT64_C(0x7FF0000000000000);
    }
    if ((ea == 0u && ma == 0u) || (eb == 0u && mb == 0u)) return sr << 63u;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint64_t sig_a = ea ? (ma | (UINT64_C(1) << 52u)) : ma;
    uint64_t sig_b = eb ? (mb | (UINT64_C(1) << 52u)) : mb;
    while ((sig_a & (UINT64_C(1) << 52u)) == 0u) { sig_a <<= 1u; exp_a--; }
    while ((sig_b & (UINT64_C(1) << 52u)) == 0u) { sig_b <<= 1u; exp_b--; }
    uint64_t plo = 0u, phi = 0u;
    u64_mul_wide(sig_a, sig_b, &plo, &phi);
    int32_t exp = exp_a + exp_b - 1023;
    /* Product is 105 or 106 bits (bit 104 or 105 set -> phi bit 40 or 41). */
    uint64_t sig = 0u;
    if (phi & (UINT64_C(1) << 41u)) {
        /* Shift right by 50 bits to place bit 105 at bit 55 */
        sig = (phi << 14u) | (plo >> 50u) | ((plo & ((UINT64_C(1) << 50u) - 1u)) ? 1u : 0u);
        exp++;
    } else {
        /* Shift right by 49 bits to place bit 104 at bit 55 */
        sig = (phi << 15u) | (plo >> 49u) | ((plo & ((UINT64_C(1) << 49u) - 1u)) ? 1u : 0u);
    }
    return pack_df(sr, exp, sig);
}

uint64_t __divdf3(uint64_t a, uint64_t b) {
    uint64_t sr = (a ^ b) >> 63u;
    uint32_t ea = (uint32_t)((a >> 52u) & 0x7FFu), eb = (uint32_t)((b >> 52u) & 0x7FFu);
    uint64_t ma = a & UINT64_C(0x000FFFFFFFFFFFFF), mb = b & UINT64_C(0x000FFFFFFFFFFFFF);
    if ((ea == 2047u && ma != 0u) || (eb == 2047u && mb != 0u)) return UINT64_C(0x7FF8000000000000);
    if (ea == 2047u) {
        if (eb == 2047u) return UINT64_C(0x7FF8000000000000);
        return (sr << 63u) | UINT64_C(0x7FF0000000000000);
    }
    if (eb == 2047u) return sr << 63u;
    if (eb == 0u && mb == 0u) {
        if (ea == 0u && ma == 0u) return UINT64_C(0x7FF8000000000000);
        return (sr << 63u) | UINT64_C(0x7FF0000000000000);
    }
    if (ea == 0u && ma == 0u) return sr << 63u;
    int32_t exp_a = ea ? (int32_t)ea : 1;
    int32_t exp_b = eb ? (int32_t)eb : 1;
    uint64_t sig_a = ea ? (ma | (UINT64_C(1) << 52u)) : ma;
    uint64_t sig_b = eb ? (mb | (UINT64_C(1) << 52u)) : mb;
    while ((sig_a & (UINT64_C(1) << 52u)) == 0u) { sig_a <<= 1u; exp_a--; }
    while ((sig_b & (UINT64_C(1) << 52u)) == 0u) { sig_b <<= 1u; exp_b--; }
    int32_t exp = exp_a - exp_b + 1023;
    if (sig_a < sig_b) { sig_a <<= 1u; exp--; }
    uint64_t q = 0u, rem = sig_a;
    for (int8_t i = 55; i >= 0; --i) {
        if (rem >= sig_b) {
            rem -= sig_b;
            q |= (UINT64_C(1) << (uint8_t)i);
        }
        rem <<= 1u;
    }
    if (rem != 0u) q |= 1u;
    return pack_df(sr, exp, q);
}

int8_t __pir_fcmp_df(uint64_t a, uint64_t b) {
    uint64_t aa = a & UINT64_C(0x7FFFFFFFFFFFFFFF), ba = b & UINT64_C(0x7FFFFFFFFFFFFFFF);
    if (aa > UINT64_C(0x7FF0000000000000) || ba > UINT64_C(0x7FF0000000000000)) return 2;
    if (aa == 0u && ba == 0u) return 0;
    if (a == b) return 0;
    uint64_t sa = a >> 63u, sb = b >> 63u;
    if (sa != sb) return sa ? -1 : 1;
    if (sa) return (aa > ba) ? -1 : 1;
    return (aa < ba) ? -1 : 1;
}

/* --- Conversions (fpext, fptrunc, sitofp, uitofp, fptosi, fptoui) --- */
uint64_t __extendsfdf2(uint32_t a);
uint32_t __truncdfsf2(uint64_t a);
uint32_t __floatsisf(int32_t v);
uint32_t __floatunsisf(uint32_t v);
uint64_t __floatsidf(int32_t v);
uint64_t __floatunsidf(uint32_t v);
uint32_t __floatdisf(int64_t v);
uint32_t __floatundisf(uint64_t v);
uint64_t __floatdidf(int64_t v);
uint64_t __floatundidf(uint64_t v);
int32_t  __fixsfsi(uint32_t a);
uint32_t __fixunssfsi(uint32_t a);
int32_t  __fixdfsi(uint64_t a);
uint32_t __fixunsdfsi(uint64_t a);
int64_t  __fixsfdi(uint32_t a);
uint64_t __fixunssfdi(uint32_t a);
int64_t  __fixdfdi(uint64_t a);
uint64_t __fixunsdfdi(uint64_t a);

uint64_t __extendsfdf2(uint32_t a) {
    uint64_t sign = (uint64_t)(a >> 31u);
    uint32_t ea = (a >> 23u) & 0xFFu;
    uint32_t ma = a & UINT32_C(0x7FFFFF);
    if (ea == 255u) {
        if (ma != 0u) return UINT64_C(0x7FF8000000000000);
        return (sign << 63u) | UINT64_C(0x7FF0000000000000);
    }
    if (ea == 0u) {
        if (ma == 0u) return sign << 63u;
        int32_t exp = 1 - 127 + 1023;
        uint64_t m = (uint64_t)ma;
        while ((m & UINT64_C(0x800000)) == 0u) { m <<= 1u; exp--; }
        return (sign << 63u) | ((uint64_t)exp << 52u) | ((m & UINT64_C(0x7FFFFF)) << 29u);
    }
    uint64_t exp = (uint64_t)(ea + (1023u - 127u));
    return (sign << 63u) | (exp << 52u) | ((uint64_t)ma << 29u);
}

uint32_t __truncdfsf2(uint64_t a) {
    uint32_t sign = (uint32_t)(a >> 63u);
    uint32_t ea = (uint32_t)((a >> 52u) & 0x7FFu);
    uint64_t ma = a & UINT64_C(0x000FFFFFFFFFFFFF);
    if (ea == 2047u) {
        if (ma != 0u) return UINT32_C(0x7FC00000);
        return (sign << 31u) | UINT32_C(0x7F800000);
    }
    if (ea == 0u && ma == 0u) return sign << 31u;
    int32_t exp = (int32_t)ea - 1023 + 127;
    uint64_t sig64 = ea ? (ma | (UINT64_C(1) << 52u)) : ma;
    uint32_t sig = (uint32_t)rshift_sticky64(sig64, 26u);
    return pack_sf(sign, exp, sig);
}

uint32_t __floatunsisf(uint32_t v) {
    if (v == 0u) return 0u;
    uint8_t lz = clz32(v);
    int32_t exp = 127 + (31 - (int32_t)lz);
    uint32_t norm = v << lz; /* bit 31 set */
    uint32_t sig = rshift_sticky32(norm, 5u); /* bit 26 set */
    return pack_sf(0u, exp, sig);
}

uint32_t __floatsisf(int32_t v) {
    if (v == 0) return 0u;
    if (v < 0) return __floatunsisf(0u - (uint32_t)v) | UINT32_C(0x80000000);
    return __floatunsisf((uint32_t)v);
}

uint64_t __floatunsidf(uint32_t v) {
    if (v == 0u) return 0u;
    uint8_t lz = clz32(v);
    int32_t exp = 1023 + (31 - (int32_t)lz);
    uint64_t sig = (uint64_t)(v << lz) << 24u; /* bit 55 set */
    return pack_df(0u, exp, sig);
}

uint64_t __floatsidf(int32_t v) {
    if (v == 0) return 0u;
    if (v < 0) return __floatunsidf(0u - (uint32_t)v) | (UINT64_C(1) << 63u);
    return __floatunsidf((uint32_t)v);
}

uint32_t __floatundisf(uint64_t v) {
    if (v == 0u) return 0u;
    uint8_t lz = clz64(v);
    int32_t exp = 127 + (63 - (int32_t)lz);
    uint64_t norm = v << lz; /* bit 63 set */
    uint32_t sig = (uint32_t)rshift_sticky64(norm, 37u); /* bit 26 set */
    return pack_sf(0u, exp, sig);
}

uint32_t __floatdisf(int64_t v) {
    if (v == 0) return 0u;
    if (v < 0) return __floatundisf(0u - (uint64_t)v) | UINT32_C(0x80000000);
    return __floatundisf((uint64_t)v);
}

uint64_t __floatundidf(uint64_t v) {
    if (v == 0u) return 0u;
    uint8_t lz = clz64(v);
    int32_t exp = 1023 + (63 - (int32_t)lz);
    uint64_t norm = v << lz; /* bit 63 set */
    uint64_t sig = rshift_sticky64(norm, 8u); /* bit 55 set */
    return pack_df(0u, exp, sig);
}

uint64_t __floatdidf(int64_t v) {
    if (v == 0) return 0u;
    if (v < 0) return __floatundidf(0u - (uint64_t)v) | (UINT64_C(1) << 63u);
    return __floatundidf((uint64_t)v);
}

static uint64_t sf_to_u64_trunc(uint32_t a, uint8_t *out_neg) {
    *out_neg = (uint8_t)(a >> 31u);
    uint32_t ea = (a >> 23u) & 0xFFu;
    uint32_t ma = a & UINT32_C(0x7FFFFF);
    if (ea < 127u) return 0u;
    int32_t exp = (int32_t)ea - 127;
    if (exp >= 64) return UINT64_MAX;
    uint64_t sig = (uint64_t)(ma | UINT32_C(0x800000));
    if (exp >= 23) return sig << (uint32_t)(exp - 23);
    return sig >> (uint32_t)(23 - exp);
}

static uint64_t df_to_u64_trunc(uint64_t a, uint8_t *out_neg) {
    *out_neg = (uint8_t)(a >> 63u);
    uint32_t ea = (uint32_t)((a >> 52u) & 0x7FFu);
    uint64_t ma = a & UINT64_C(0x000FFFFFFFFFFFFF);
    if (ea < 1023u) return 0u;
    int32_t exp = (int32_t)ea - 1023;
    if (exp >= 64) return UINT64_MAX;
    uint64_t sig = ma | (UINT64_C(1) << 52u);
    if (exp >= 52) return sig << (uint32_t)(exp - 52);
    return sig >> (uint32_t)(52 - exp);
}

uint32_t __fixunssfsi(uint32_t a) {
    uint8_t neg = 0u;
    if ((a & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) return 0u;
    uint64_t m = sf_to_u64_trunc(a, &neg);
    if (neg) return 0u;
    return (m > UINT32_MAX) ? UINT32_MAX : (uint32_t)m;
}

int32_t __fixsfsi(uint32_t a) {
    uint8_t neg = 0u;
    if ((a & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) return 0;
    uint64_t m = sf_to_u64_trunc(a, &neg);
    if (neg) return (m >= UINT64_C(0x80000000)) ? INT32_MIN : (int32_t)(0u - (uint32_t)m);
    return (m >= UINT64_C(0x7FFFFFFF)) ? INT32_MAX : (int32_t)m;
}

uint32_t __fixunsdfsi(uint64_t a) {
    uint8_t neg = 0u;
    if ((a & UINT64_C(0x7FFFFFFFFFFFFFFF)) > UINT64_C(0x7FF0000000000000)) return 0u;
    uint64_t m = df_to_u64_trunc(a, &neg);
    if (neg) return 0u;
    return (m > UINT32_MAX) ? UINT32_MAX : (uint32_t)m;
}

int32_t __fixdfsi(uint64_t a) {
    uint8_t neg = 0u;
    if ((a & UINT64_C(0x7FFFFFFFFFFFFFFF)) > UINT64_C(0x7FF0000000000000)) return 0;
    uint64_t m = df_to_u64_trunc(a, &neg);
    if (neg) return (m >= UINT64_C(0x80000000)) ? INT32_MIN : (int32_t)(0u - (uint32_t)m);
    return (m >= UINT64_C(0x7FFFFFFF)) ? INT32_MAX : (int32_t)m;
}

uint64_t __fixunssfdi(uint32_t a) {
    uint8_t neg = 0u;
    if ((a & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) return 0u;
    uint64_t m = sf_to_u64_trunc(a, &neg);
    return neg ? 0u : m;
}

int64_t __fixsfdi(uint32_t a) {
    uint8_t neg = 0u;
    if ((a & UINT32_C(0x7FFFFFFF)) > UINT32_C(0x7F800000)) return 0;
    uint64_t m = sf_to_u64_trunc(a, &neg);
    if (neg) return (m >= (UINT64_C(1) << 63u)) ? INT64_MIN : (int64_t)(0u - m);
    return (m >= UINT64_C(0x7FFFFFFFFFFFFFFF)) ? INT64_MAX : (int64_t)m;
}

uint64_t __fixunsdfdi(uint64_t a) {
    uint8_t neg = 0u;
    if ((a & UINT64_C(0x7FFFFFFFFFFFFFFF)) > UINT64_C(0x7FF0000000000000)) return 0u;
    uint64_t m = df_to_u64_trunc(a, &neg);
    return neg ? 0u : m;
}

int64_t __fixdfdi(uint64_t a) {
    uint8_t neg = 0u;
    if ((a & UINT64_C(0x7FFFFFFFFFFFFFFF)) > UINT64_C(0x7FF0000000000000)) return 0;
    uint64_t m = df_to_u64_trunc(a, &neg);
    if (neg) return (m >= (UINT64_C(1) << 63u)) ? INT64_MIN : (int64_t)(0u - m);
    return (m >= UINT64_C(0x7FFFFFFFFFFFFFFF)) ? INT64_MAX : (int64_t)m;
}

/* =========================================================================
 * 3. 32-bit & 64-bit Interrupt-Masked Atomics
 * ========================================================================= */

void    __pir_atomic_load_4(const volatile uint32_t *p, uint32_t *out);
void    __pir_atomic_store_4(volatile uint32_t *p, uint16_t lo, uint16_t hi);
void    __pir_atomic_rmw_4(volatile uint32_t *p, uint32_t *out, uint16_t op, uint16_t lo, uint16_t hi);
uint8_t __pir_atomic_cmpxchg_4(volatile uint32_t *p, uint32_t *out,
                               uint16_t elo, uint16_t ehi, uint16_t nlo, uint16_t nhi);

void __pir_atomic_load_4(const volatile uint32_t *p, uint32_t *out) {
    uint8_t s = avr_irq_save();
    *out = *p;
    avr_irq_restore(s);
}

void __pir_atomic_store_4(volatile uint32_t *p, uint16_t lo, uint16_t hi) {
    uint8_t s = avr_irq_save();
    *p = (uint32_t)lo | ((uint32_t)hi << 16u);
    avr_irq_restore(s);
}

void __pir_atomic_rmw_4(volatile uint32_t *p, uint32_t *out, uint16_t op, uint16_t lo, uint16_t hi) {
    uint8_t s = avr_irq_save();
    uint32_t old = *p;
    uint32_t rhs = (uint32_t)lo | ((uint32_t)hi << 16u);
    uint32_t nv  = old;
    if (op == 0u)      nv = old + rhs;
    else if (op == 1u) nv = old - rhs;
    else if (op == 2u) nv = old & rhs;
    else if (op == 3u) nv = old | rhs;
    else if (op == 4u) nv = old ^ rhs;
    else if (op == 5u) nv = rhs;
    *out = old;
    *p   = nv;
    avr_irq_restore(s);
}

uint8_t __pir_atomic_cmpxchg_4(volatile uint32_t *p, uint32_t *out,
                               uint16_t elo, uint16_t ehi, uint16_t nlo, uint16_t nhi) {
    uint8_t s = avr_irq_save();
    uint32_t old = *p;
    uint32_t exp = (uint32_t)elo | ((uint32_t)ehi << 16u);
    uint32_t des = (uint32_t)nlo | ((uint32_t)nhi << 16u);
    *out = old;
    uint8_t ok = 0u;
    if (old == exp) { *p = des; ok = 1u; }
    avr_irq_restore(s);
    return ok;
}

void    __pir_atomic_load_8(const volatile uint64_t *p, uint64_t *out);
void    __pir_atomic_store_8(volatile uint64_t *p, uint32_t lo, uint32_t hi);
void    __pir_atomic_rmw_8(volatile uint64_t *p, uint64_t *out, uint32_t op, uint32_t lo, uint32_t hi);
uint8_t __pir_atomic_cmpxchg_8(volatile uint64_t *p, uint64_t *out,
                               uint32_t elo, uint32_t ehi, uint32_t nlo, uint32_t nhi);

void __pir_atomic_load_8(const volatile uint64_t *p, uint64_t *out) {
    uint8_t s = avr_irq_save();
    *out = *p;
    avr_irq_restore(s);
}

void __pir_atomic_store_8(volatile uint64_t *p, uint32_t lo, uint32_t hi) {
    uint8_t s = avr_irq_save();
    *p = (uint64_t)lo | ((uint64_t)hi << 32u);
    avr_irq_restore(s);
}

void __pir_atomic_rmw_8(volatile uint64_t *p, uint64_t *out, uint32_t op, uint32_t lo, uint32_t hi) {
    uint8_t s = avr_irq_save();
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
    avr_irq_restore(s);
}

uint8_t __pir_atomic_cmpxchg_8(volatile uint64_t *p, uint64_t *out,
                               uint32_t elo, uint32_t ehi, uint32_t nlo, uint32_t nhi) {
    uint8_t s = avr_irq_save();
    uint64_t old = *p;
    uint64_t exp = (uint64_t)elo | ((uint64_t)ehi << 32u);
    uint64_t des = (uint64_t)nlo | ((uint64_t)nhi << 32u);
    *out = old;
    uint8_t ok = 0u;
    if (old == exp) { *p = des; ok = 1u; }
    avr_irq_restore(s);
    return ok;
}

/* =========================================================================
 * 4. Shadow-Stack Exception Unwinder
 * ========================================================================= */

typedef struct PirExcFrame {
    struct PirExcFrame *prev;
    void               *exn;
    uint8_t             jb[32];
} PirExcFrame;

__attribute__((weak)) PirExcFrame *__pir_exc_head = NULL;

void __pir_unwind_resume(void *exn) __attribute__((noreturn));
void __pir_unwind_resume(void *exn) {
    PirExcFrame *f = __pir_exc_head;
    if (!f) {
        _pir_avr_exit(132);
    }
    __pir_exc_head = f->prev;
    f->exn = exn;
    __pir_longjmp(f->jb, 1);
}

/* =========================================================================
 * 5. Minimal Freestanding C Library Shims
 * ========================================================================= */

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int32_t c, size_t n);
int32_t memcmp(const void *a, const void *b, size_t n);
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

void *memset(void *dst, int32_t c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0u; i < n; ++i) d[i] = (uint8_t)c;
    return dst;
}

int32_t memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0u; i < n; ++i) {
        if (x[i] != y[i]) return (int32_t)x[i] - (int32_t)y[i];
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
    if (sz > (size_t)UINT16_MAX - 2u || heap_cur >= __heap_end) return NULL;
    size_t total = sz + 2u;
    if ((size_t)(__heap_end - heap_cur) < total) return NULL;
    uint16_t *hdr = (uint16_t *)heap_cur;
    *hdr = (uint16_t)sz;
    void *res = heap_cur + 2u;
    heap_cur += total;
    return res;
}

void *calloc(size_t n, size_t sz) {
    if (n != 0u && sz > (size_t)UINT16_MAX / n) return NULL;
    size_t tot = n * sz;
    void *p = malloc(tot);
    if (p) memset(p, 0, tot);
    return p;
}

void *realloc(void *p, size_t sz) {
    if (!p) return malloc(sz);
    uint16_t old_sz = *((uint16_t *)((uint8_t *)p - 2u));
    void *np = malloc(sz);
    if (!np) return NULL;
    memcpy(np, p, old_sz < sz ? old_sz : sz);
    return np;
}

void free(void *p) { (void)p; }

int32_t putchar(int32_t c);
int32_t puts(const char *s);
int32_t printf(const char *fmt, ...);
void    exit(int32_t code) __attribute__((noreturn));
void    abort(void) __attribute__((noreturn));

int32_t putchar(int32_t c) {
    _pir_avr_putc((int16_t)(c & 0xFF));
    return c & 0xFF;
}

int32_t puts(const char *s) {
    int32_t n = 0;
    while (*s) { _pir_avr_putc((unsigned char)*s++); n++; }
    _pir_avr_putc('\n');
    return n + 1;
}

static int32_t print_uint(uint64_t v, uint16_t base, uint8_t upper) {
    char buf[24];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int32_t n = 0;
    if (v == 0u) { _pir_avr_putc('0'); return 1; }
    while (v > 0u) {
        uint64_t q = 0u, r = 0u;
        u64_divmod(v, base, &q, &r);
        buf[n++] = digs[r];
        v = q;
    }
    for (int32_t i = n - 1; i >= 0; --i) _pir_avr_putc((unsigned char)buf[i]);
    return n;
}

/* Pure-integer binary64 formatter using our own soft-float helpers. */
static int32_t print_double_bits(uint64_t bits, int16_t prec) {
    int32_t n = 0;
    uint64_t abs_b = bits & UINT64_C(0x7FFFFFFFFFFFFFFF);
    if (abs_b > UINT64_C(0x7FF0000000000000)) {
        const char *s = "nan";
        while (*s) { _pir_avr_putc((unsigned char)*s++); n++; }
        return n;
    }
    if (bits >> 63u) {
        _pir_avr_putc('-');
        n++;
        bits = abs_b;
    }
    if (abs_b == UINT64_C(0x7FF0000000000000)) {
        const char *s = "inf";
        while (*s) { _pir_avr_putc((unsigned char)*s++); n++; }
        return n;
    }
    if (prec < 0) prec = 6;
    if (prec > 9) prec = 9;
    uint32_t iscale = 1u;
    for (int16_t i = 0; i < prec; ++i) iscale = __mulsi3(iscale, 10u);
    uint64_t ipart = __fixunsdfdi(bits);
    uint64_t frac  = __subdf3(bits, __floatundidf(ipart));
    uint64_t scaled = __adddf3(__muldf3(frac, __floatunsidf(iscale)), UINT64_C(0x3FE0000000000000)); /* +0.5 */
    uint32_t fpart = __fixunsdfsi(scaled);
    if (fpart >= iscale) {
        fpart -= iscale;
        ipart++;
    }
    n += print_uint(ipart, 10u, 0u);
    if (prec > 0) {
        _pir_avr_putc('.');
        n++;
        char fbuf[12];
        for (int16_t i = prec - 1; i >= 0; --i) {
            uint32_t q, r;
            u32_divmod(fpart, 10u, &q, &r);
            fbuf[i] = (char)('0' + r);
            fpart = q;
        }
        for (int16_t i = 0; i < prec; ++i) {
            _pir_avr_putc((unsigned char)fbuf[i]);
            n++;
        }
    }
    return n;
}

int32_t printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int32_t total = 0;
    for (const char *s = fmt; *s; ++s) {
        if (*s != '%') { _pir_avr_putc((unsigned char)*s); total++; continue; }
        s++;
        if (*s == '%') { _pir_avr_putc('%'); total++; continue; }
        uint8_t lmod = 0u; /* 0=32b default, 1=h (16b), 2=ll (64b) */
        int16_t prec = -1;
        while (*s == '-' || *s == '+' || *s == ' ' || *s == '0' || *s == '#') s++;
        while (*s >= '0' && *s <= '9') s++;
        if (*s == '.') {
            s++;
            prec = 0;
            while (*s >= '0' && *s <= '9') { prec = (int16_t)(prec * 10 + (*s - '0')); s++; }
        }
        if (*s == 'h') { s++; lmod = 1u; if (*s == 'h') s++; }
        else if (*s == 'l') { s++; if (*s == 'l') { lmod = 2u; s++; } }
        else if (*s == 'z') { s++; lmod = 1u; }
        if (*s == 'd' || *s == 'i') {
            int64_t v = (lmod == 2u) ? va_arg(ap, int64_t)
                      : (lmod == 1u) ? (int64_t)va_arg(ap, int16_t)
                                     : (int64_t)va_arg(ap, int32_t);
            if (v < 0) { _pir_avr_putc('-'); total++; total += print_uint(0u - (uint64_t)v, 10u, 0u); }
            else       { total += print_uint((uint64_t)v, 10u, 0u); }
        } else if (*s == 'u') {
            uint64_t v = (lmod == 2u) ? va_arg(ap, uint64_t)
                       : (lmod == 1u) ? (uint64_t)va_arg(ap, uint16_t)
                                      : (uint64_t)va_arg(ap, uint32_t);
            total += print_uint(v, 10u, 0u);
        } else if (*s == 'x' || *s == 'X') {
            uint64_t v = (lmod == 2u) ? va_arg(ap, uint64_t)
                       : (lmod == 1u) ? (uint64_t)va_arg(ap, uint16_t)
                                      : (uint64_t)va_arg(ap, uint32_t);
            total += print_uint(v, 16u, (*s == 'X') ? 1u : 0u);
        } else if (*s == 'c') {
            uint32_t ch = va_arg(ap, uint32_t);
            _pir_avr_putc((int16_t)(ch & 0xFFu));
            total++;
        } else if (*s == 's') {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            while (*str) { _pir_avr_putc((unsigned char)*str++); total++; }
        } else if (*s == 'p') {
            uint16_t ptr = (uint16_t)(uintptr_t)va_arg(ap, void *);
            _pir_avr_putc('0'); _pir_avr_putc('x'); total += 2;
            total += print_uint((uint64_t)ptr, 16u, 0u);
        } else if (*s == 'f' || *s == 'F' || *s == 'g' || *s == 'G') {
            uint64_t dbits = va_arg(ap, uint64_t);
            total += print_double_bits(dbits, prec);
        }
    }
    va_end(ap);
    return total;
}

void exit(int32_t code) { _pir_avr_exit((int16_t)(code & 0xFF)); }
void abort(void)        { _pir_avr_exit(132); }
