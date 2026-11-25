/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.17 — Adversarial test for x86-64 atomic
 * lock cmpxchg / lock xadd / xchg under extreme register pressure (14+ live GP
 * regs forcing spills into r10/r11/r15), RAX-returning ptr & val contention,
 * 8/16/32/64-bit subword upper-bit hygiene, discarded CAS bool return, and
 * self-referential atomic_exchange(&p, (uintptr_t)&p).
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "c17_test.h"

/* Force ptr and val to arrive in RAX via non-inlined function returns */
static _Atomic uint8_t  *ret_p8(_Atomic uint8_t *p)   { volatile uintptr_t x = (uintptr_t)p; return (_Atomic uint8_t *)x; }
static _Atomic uint16_t *ret_p16(_Atomic uint16_t *p) { volatile uintptr_t x = (uintptr_t)p; return (_Atomic uint16_t *)x; }
static _Atomic uint32_t *ret_p32(_Atomic uint32_t *p) { volatile uintptr_t x = (uintptr_t)p; return (_Atomic uint32_t *)x; }
static _Atomic uint64_t *ret_p64(_Atomic uint64_t *p) { volatile uintptr_t x = (uintptr_t)p; return (_Atomic uint64_t *)x; }

static uint8_t  ret_v8(uint8_t v)   { volatile uint8_t x = v; return x; }
static uint16_t ret_v16(uint16_t v) { volatile uint16_t x = v; return x; }
static uint32_t ret_v32(uint32_t v) { volatile uint32_t x = v; return x; }
static uint64_t ret_v64(uint64_t v) { volatile uint64_t x = v; return x; }

/* 14+ live GP variables spanning every atomic RMW (AND/OR/XOR/ADD/SUB/XCHG) & CAS */
static uint64_t stress_atomic_pressure(_Atomic uint8_t *p8, _Atomic uint16_t *p16,
                                       _Atomic uint32_t *p32, _Atomic uint64_t *p64,
                                       uint64_t seed) {
    uint64_t l0 = seed + 1u,  l1 = seed + 2u,  l2 = seed + 3u,  l3 = seed + 4u;
    uint64_t l4 = seed + 5u,  l5 = seed + 6u,  l6 = seed + 7u,  l7 = seed + 8u;
    uint64_t l8 = seed + 9u,  l9 = seed + 10u, l10 = seed + 11u, l11 = seed + 12u;
    uint64_t l12 = seed + 13u, l13 = seed + 14u;

    /* 8-bit RMWs (AND/OR/XOR use lock cmpxchg loop with fixed rax + early-clobber tmp!) */
    uint8_t o8_or  = atomic_fetch_or(ret_p8(p8), ret_v8(0x0Fu));
    uint8_t o8_and = atomic_fetch_and(ret_p8(p8), ret_v8(0xF3u));
    uint8_t o8_xor = atomic_fetch_xor(ret_p8(p8), ret_v8(0xAAu));
    uint8_t o8_add = atomic_fetch_add(ret_p8(p8), ret_v8(5u));
    uint8_t o8_sub = atomic_fetch_sub(ret_p8(p8), ret_v8(2u));

    /* 16-bit RMWs */
    uint16_t o16_or  = atomic_fetch_or(ret_p16(p16), ret_v16(0x00FFu));
    uint16_t o16_and = atomic_fetch_and(ret_p16(p16), ret_v16(0xFF0Fu));
    uint16_t o16_xor = atomic_fetch_xor(ret_p16(p16), ret_v16(0x5555u));
    uint16_t o16_add = atomic_fetch_add(ret_p16(p16), ret_v16(10u));
    uint16_t o16_sub = atomic_fetch_sub(ret_p16(p16), ret_v16(3u));

    /* 32-bit RMWs */
    uint32_t o32_or  = atomic_fetch_or(ret_p32(p32), ret_v32(0x0000FFFFu));
    uint32_t o32_and = atomic_fetch_and(ret_p32(p32), ret_v32(0xFFFF00FFu));
    uint32_t o32_xor = atomic_fetch_xor(ret_p32(p32), ret_v32(0x12345678u));
    uint32_t o32_add = atomic_fetch_add(ret_p32(p32), ret_v32(100u));
    uint32_t o32_sub = atomic_fetch_sub(ret_p32(p32), ret_v32(25u));

    /* 64-bit RMWs */
    uint64_t o64_or  = atomic_fetch_or(ret_p64(p64), ret_v64(0x00000000FFFFFFFFULL));
    uint64_t o64_and = atomic_fetch_and(ret_p64(p64), ret_v64(0xFFFFFFFF0000FFFFULL));
    uint64_t o64_xor = atomic_fetch_xor(ret_p64(p64), ret_v64(0x1122334455667788ULL));
    uint64_t o64_add = atomic_fetch_add(ret_p64(p64), ret_v64(1000ULL));
    uint64_t o64_sub = atomic_fetch_sub(ret_p64(p64), ret_v64(250ULL));

    /* Use all 14 live values AFTER all atomic ops so none are dead */
    uint64_t live_sum = l0 + l1 + l2 + l3 + l4 + l5 + l6
                      + l7 + l8 + l9 + l10 + l11 + l12 + l13;
    return live_sum
         + (uint64_t)o8_or + (uint64_t)o8_and + (uint64_t)o8_xor + (uint64_t)o8_add + (uint64_t)o8_sub
         + (uint64_t)o16_or + (uint64_t)o16_and + (uint64_t)o16_xor + (uint64_t)o16_add + (uint64_t)o16_sub
         + (uint64_t)o32_or + (uint64_t)o32_and + (uint64_t)o32_xor + (uint64_t)o32_add + (uint64_t)o32_sub
         + o64_or + o64_and + o64_xor + o64_add + o64_sub;
}

int main(void) {
    C17_BEGIN("c717_bug_x64_atomic_regpressure",
              "x86-64 atomic RMW/CAS under high GP spill pressure & RAX contention");

    /* 1. RMW pressure across 8/16/32/64-bit */
    {
        _Atomic uint8_t  a8  = 0xA0u;
        _Atomic uint16_t a16 = 0x1200u;
        _Atomic uint32_t a32 = 0x87650000u;
        _Atomic uint64_t a64 = 0xFEDCBA9800000000ULL;

        uint64_t res = stress_atomic_pressure(&a8, &a16, &a32, &a64, (uint64_t)c17_opaque_int(10));

        /* Trace expected transitions */
        /* a8: 0xA0 -> |0x0F=0xAF -> &0xF3=0xA3 -> ^0xAA=0x09 -> +5=0x0E -> -2=0x0C */
        C17_CHECK_UEQ(atomic_load(&a8), 0x0Cu);
        /* a16: 0x1200 -> |0x00FF=0x12FF -> &0xFF0F=0x120F -> ^0x5555=0x475A -> +10=0x4764 -> -3=0x4761 */
        C17_CHECK_UEQ(atomic_load(&a16), 0x4761u);
        /* a32: 0x87650000 -> |0xFFFF=0x8765FFFF -> &0xFFFF00FF=0x876500FF -> ^0x12345678=0x95515687 -> +100 -> -25 */
        C17_CHECK_UEQ(atomic_load(&a32), 0x95515687u + 75u);
        /* a64: 0xFEDCBA9800000000 -> |0xFFFFFFFF -> &0xFFFFFFFF0000FFFF -> ^0x1122334455667788 -> +750 */
        uint64_t e64 = (0xFEDCBA980000FFFFULL ^ 0x1122334455667788ULL) + 750ULL;
        C17_CHECK_UEQ(atomic_load(&a64), e64);

        uint64_t want_sum = (14ULL * 10ULL + 105ULL)
            + (0xA0ULL + 0xAFULL + 0xA3ULL + 0x09ULL + 0x0EULL)
            + (0x1200ULL + 0x12FFULL + 0x120FULL + 0x475AULL + 0x4764ULL)
            + (0x87650000ULL + 0x8765FFFFULL + 0x876500FFULL + 0x95515687ULL + (0x95515687ULL + 100ULL))
            + (0xFEDCBA9800000000ULL + 0xFEDCBA98FFFFFFFFULL + 0xFEDCBA980000FFFFULL
               + (0xFEDCBA980000FFFFULL ^ 0x1122334455667788ULL)
               + ((0xFEDCBA980000FFFFULL ^ 0x1122334455667788ULL) + 1000ULL));
        C17_CHECK_UEQ(res, want_sum);
    }

    /* 2. CAS success & failure with BOTH used and discarded bool result across 8/16/32/64-bit */
    {
        _Atomic uint8_t  c8  = 0x7Eu;
        _Atomic uint16_t c16 = 0xBEEFu;
        _Atomic uint32_t c32 = 0xCAFEBABEu;
        _Atomic uint64_t c64 = 0x0123456789ABCDEFULL;

        /* 8-bit fail (discarded bool!) -> exp updated cleanly without upper garbage */
        uint8_t exp8 = 0x11u;
        (void)atomic_compare_exchange_strong(ret_p8(&c8), &exp8, ret_v8(0x99u));
        C17_CHECK_UEQ(exp8, 0x7Eu);
        C17_CHECK_UEQ(atomic_load(&c8), 0x7Eu);
        bool ok8 = atomic_compare_exchange_strong(ret_p8(&c8), &exp8, ret_v8(0x99u));
        C17_CHECK(ok8);
        C17_CHECK_UEQ(atomic_load(&c8), 0x99u);

        /* 16-bit fail + succeed */
        uint16_t exp16 = 0x1234u;
        (void)atomic_compare_exchange_strong(ret_p16(&c16), &exp16, ret_v16(0x4321u));
        C17_CHECK_UEQ(exp16, 0xBEEFu);
        bool ok16 = atomic_compare_exchange_strong(ret_p16(&c16), &exp16, ret_v16(0x4321u));
        C17_CHECK(ok16);
        C17_CHECK_UEQ(atomic_load(&c16), 0x4321u);

        /* 32-bit fail + succeed */
        uint32_t exp32 = 0x11111111u;
        bool fail32 = atomic_compare_exchange_strong(ret_p32(&c32), &exp32, ret_v32(0x22222222u));
        C17_CHECK(!fail32);
        C17_CHECK_UEQ(exp32, 0xCAFEBABEu);
        (void)atomic_compare_exchange_strong(ret_p32(&c32), &exp32, ret_v32(0x22222222u));
        C17_CHECK_UEQ(atomic_load(&c32), 0x22222222u);

        /* 64-bit fail + succeed */
        uint64_t exp64 = 0xDEADBEEFCAFEBABEULL;
        bool fail64 = atomic_compare_exchange_strong(ret_p64(&c64), &exp64, ret_v64(0xF00DBABE12345678ULL));
        C17_CHECK(!fail64);
        C17_CHECK_UEQ(exp64, 0x0123456789ABCDEFULL);
        bool ok64 = atomic_compare_exchange_strong(ret_p64(&c64), &exp64, ret_v64(0xF00DBABE12345678ULL));
        C17_CHECK(ok64);
        C17_CHECK_UEQ(atomic_load(&c64), 0xF00DBABE12345678ULL);
    }

    /* 3. Self-referential atomic_exchange(&slot, (uintptr_t)&slot) (ptr == val!) */
    {
        _Atomic uintptr_t slot = 0x1234u;
        uintptr_t self = (uintptr_t)&slot;
        uintptr_t old = atomic_exchange(&slot, self);
        C17_CHECK_UEQ(old, 0x1234u);
        C17_CHECK_UEQ(atomic_load(&slot), self);
    }

    C17_END();
}
