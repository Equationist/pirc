/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §7.17 — Real-world compiler bug regressions:
 *   - LLVM AArch64 atomic LL/SC + large stack frame + high register pressure +
 *     variable shifts (scratch/base register clobbering across atomic ops)
 */
#include "c17_test.h"

#ifdef __STDC_NO_ATOMICS__
int main(void) {
    C17_BEGIN("c717_bug_llvm_arm64_atomic_regalloc",
              "LLVM AArch64 atomic + large stack + high register pressure");
    C17_NOT_APPLICABLE("__STDC_NO_ATOMICS__ is defined");
}
#else
#include <stdatomic.h>
#include <stdint.h>

static uint64_t heavy_atomic_frame(
    _Atomic uint8_t *a8,
    _Atomic uint16_t *a16,
    _Atomic uint32_t *a32,
    _Atomic uint64_t *a64,
    uint64_t seed,
    unsigned sh)
{
    /* > 4KB local stack frame forces large immediate offsets on AArch64 */
    uint64_t buf[600];
    buf[0] = seed;
    buf[599] = seed ^ 0x55AA55AA55AA55AAULL;

    /* High register pressure: 12 live values held across multiple atomic ops */
    uint64_t v0 = seed + 1;
    uint64_t v1 = seed + 2;
    uint64_t v2 = (seed << sh) ^ 0x1111ULL;
    uint64_t v3 = (seed >> sh) ^ 0x2222ULL;
    uint64_t v4 = v0 + v1;
    uint64_t v5 = v2 ^ v3;
    uint64_t v6 = v4 * 3 + 7;
    uint64_t v7 = v5 + v6;
    uint64_t v8 = buf[0] + v7;
    uint64_t v9 = buf[599] ^ v8;

    uint8_t old8 = atomic_fetch_add_explicit(a8, (uint8_t)5, memory_order_seq_cst);
    uint16_t old16 = atomic_fetch_or_explicit(a16, (uint16_t)0x0F00, memory_order_acq_rel);
    uint32_t old32 = atomic_fetch_xor_explicit(a32, (uint32_t)0x00FF00FFU, memory_order_seq_cst);

    uint64_t expected = atomic_load_explicit(a64, memory_order_acquire);
    uint64_t desired = expected + (v0 & 0xFFULL) + (1ULL << sh);
    while (!atomic_compare_exchange_weak_explicit(
               a64, &expected, desired,
               memory_order_seq_cst, memory_order_acquire)) {
        desired = expected + (v0 & 0xFFULL) + (1ULL << sh);
    }

    buf[300] = (uint64_t)old8 + (uint64_t)old16 + (uint64_t)old32 + expected;
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + buf[0] + buf[300] + buf[599];
}

int main(void) {
    C17_BEGIN("c717_bug_llvm_arm64_atomic_regalloc",
              "LLVM AArch64 atomic + large stack + high register pressure");

    _Atomic uint8_t a8 = 10;
    _Atomic uint16_t a16 = 0x0034;
    _Atomic uint32_t a32 = 0x12345678U;
    _Atomic uint64_t a64 = 1000ULL;

    uint64_t seed = c17_opaque_ull(0x100ULL);
    unsigned sh = (unsigned)c17_opaque_int(3);

    uint64_t got = heavy_atomic_frame(&a8, &a16, &a32, &a64, seed, sh);

    /* Independently compute expected result */
    uint64_t b0 = seed;
    uint64_t b599 = seed ^ 0x55AA55AA55AA55AAULL;
    uint64_t v0 = seed + 1;
    uint64_t v1 = seed + 2;
    uint64_t v2 = (seed << sh) ^ 0x1111ULL;
    uint64_t v3 = (seed >> sh) ^ 0x2222ULL;
    uint64_t v4 = v0 + v1;
    uint64_t v5 = v2 ^ v3;
    uint64_t v6 = v4 * 3 + 7;
    uint64_t v7 = v5 + v6;
    uint64_t v8 = b0 + v7;
    uint64_t v9 = b599 ^ v8;
    uint64_t b300 = 10ULL + 0x0034ULL + 0x12345678ULL + 1000ULL;
    uint64_t want = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + b0 + b300 + b599;

    C17_CHECK_UEQ(got, want);
    C17_CHECK_UEQ(atomic_load(&a8), 15U);
    C17_CHECK_UEQ(atomic_load(&a16), 0x0F34U);
    C17_CHECK_UEQ(atomic_load(&a32), 0x12CB5687U);
    C17_CHECK_UEQ(atomic_load(&a64), 1000ULL + (v0 & 0xFFULL) + (1ULL << sh));

    C17_END();
}
#endif
