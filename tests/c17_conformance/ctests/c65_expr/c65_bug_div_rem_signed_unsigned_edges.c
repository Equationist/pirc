/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.5.5, §6.3.1.1 — Adversarial signed & unsigned
 * integer division/remainder torture (GCC/LLVM/QBE/TCC/CompCert regressions):
 *  - x / -1, x % -1, negative dividends with pow2 and non-pow2 +/- divisors
 *  - INT32_MIN / INT64_MIN / UINT32_MAX / UINT64_MAX edge divisors
 *  - Narrow signed (int8_t)-128 / -1 and (int16_t)-32768 / -1 (valid in C17
 *    via integer promotion to int — MUST yield +128 / +32768, NOT trap!)
 */
#include <limits.h>
#include <stdint.h>
#include "c17_test.h"

static int32_t div_s32_m1(int32_t x) { return x / -1; }
static int32_t rem_s32_m1(int32_t x) { return x % -1; }
static int32_t div_s32_m2(int32_t x) { return x / -2; }
static int32_t rem_s32_m2(int32_t x) { return x % -2; }
static int32_t div_s32_3(int32_t x)  { return x / 3; }
static int32_t rem_s32_3(int32_t x)  { return x % 3; }
static int32_t div_s32_m7(int32_t x) { return x / -7; }
static int32_t rem_s32_m7(int32_t x) { return x % -7; }
static int64_t div_s64_m2(int64_t x) { return x / -2; }
static int64_t rem_s64_m2(int64_t x) { return x % -2; }
static int64_t div_s64_m3(int64_t x) { return x / -3; }
static int64_t rem_s64_m3(int64_t x) { return x % -3; }
static uint64_t div_u64_3(uint64_t x)  { return x / 3u; }
static uint64_t rem_u64_3(uint64_t x)  { return x % 3u; }
static uint64_t div_u64_7(uint64_t x)  { return x / 7u; }
static uint64_t rem_u64_7(uint64_t x)  { return x % 7u; }
static uint64_t div_u64_10(uint64_t x) { return x / 10u; }
static uint64_t rem_u64_10(uint64_t x) { return x % 10u; }
static uint32_t div_u32_big(uint32_t x) { return x / UINT32_C(0x80000001); }
static uint32_t rem_u32_big(uint32_t x) { return x % UINT32_C(0x80000001); }

static int div_i8_min_m1(int8_t a, int8_t b)   { return a / b; }
static int div_i16_min_m1(int16_t a, int16_t b) { return a / b; }

static int32_t (*volatile g_ds32_m1)(int32_t) = div_s32_m1;
static int32_t (*volatile g_rs32_m1)(int32_t) = rem_s32_m1;
static int32_t (*volatile g_ds32_m2)(int32_t) = div_s32_m2;
static int32_t (*volatile g_rs32_m2)(int32_t) = rem_s32_m2;
static int32_t (*volatile g_ds32_3)(int32_t)  = div_s32_3;
static int32_t (*volatile g_rs32_3)(int32_t)  = rem_s32_3;
static int32_t (*volatile g_ds32_m7)(int32_t) = div_s32_m7;
static int32_t (*volatile g_rs32_m7)(int32_t) = rem_s32_m7;
static int64_t (*volatile g_ds64_m2)(int64_t) = div_s64_m2;
static int64_t (*volatile g_rs64_m2)(int64_t) = rem_s64_m2;
static int64_t (*volatile g_ds64_m3)(int64_t) = div_s64_m3;
static int64_t (*volatile g_rs64_m3)(int64_t) = rem_s64_m3;
static uint64_t (*volatile g_du64_3)(uint64_t)  = div_u64_3;
static uint64_t (*volatile g_ru64_3)(uint64_t)  = rem_u64_3;
static uint64_t (*volatile g_du64_7)(uint64_t)  = div_u64_7;
static uint64_t (*volatile g_ru64_7)(uint64_t)  = rem_u64_7;
static uint64_t (*volatile g_du64_10)(uint64_t) = div_u64_10;
static uint64_t (*volatile g_ru64_10)(uint64_t) = rem_u64_10;
static uint32_t (*volatile g_du32_big)(uint32_t) = div_u32_big;
static uint32_t (*volatile g_ru32_big)(uint32_t) = rem_u32_big;
static int (*volatile g_di8)(int8_t, int8_t)    = div_i8_min_m1;
static int (*volatile g_di16)(int16_t, int16_t) = div_i16_min_m1;

int main(void) {
    C17_BEGIN("c65_bug_div_rem_signed_unsigned_edges",
              "Signed/unsigned div/rem edge divisors, INT_MIN, UINT64_MAX, narrow promos");

    /* 1. Signed 32-bit edge dividends & divisors */
    C17_CHECK_EQ(g_ds32_m1(-5), 5);
    C17_CHECK_EQ(g_rs32_m1(-5), 0);
    C17_CHECK_EQ(g_ds32_m1(INT32_MAX), -INT32_MAX);
    C17_CHECK_EQ(g_rs32_m1(INT32_MAX), 0);

    C17_CHECK_EQ(g_ds32_m2(-1), 0);
    C17_CHECK_EQ(g_rs32_m2(-1), -1);
    C17_CHECK_EQ(g_ds32_m2(-7), 3);
    C17_CHECK_EQ(g_rs32_m2(-7), -1);
    C17_CHECK_EQ(g_ds32_m2(INT32_MIN), 1073741824);
    C17_CHECK_EQ(g_rs32_m2(INT32_MIN), 0);

    C17_CHECK_EQ(g_ds32_3(INT32_MIN), -715827882);
    C17_CHECK_EQ(g_rs32_3(INT32_MIN), -2);
    C17_CHECK_EQ(g_ds32_m7(INT32_MIN), 306783378);
    C17_CHECK_EQ(g_rs32_m7(INT32_MIN), -2);
    C17_CHECK_EQ(g_ds32_m7(INT32_MAX), -306783378);
    C17_CHECK_EQ(g_rs32_m7(INT32_MAX), 1);

    /* 2. Signed 64-bit INT64_MIN & negative divisors */
    C17_CHECK_EQ(g_ds64_m2(INT64_MIN), INT64_C(4611686018427387904));
    C17_CHECK_EQ(g_rs64_m2(INT64_MIN), 0);
    C17_CHECK_EQ(g_ds64_m3(INT64_MIN), INT64_C(3074457345618258602));
    C17_CHECK_EQ(g_rs64_m3(INT64_MIN), -2);
    C17_CHECK_EQ(g_ds64_m3(-1), 0);
    C17_CHECK_EQ(g_rs64_m3(-1), -1);

    /* 3. Unsigned 64-bit UINT64_MAX & 32-bit large divisors */
    C17_CHECK_EQ(g_du64_3(UINT64_MAX), UINT64_C(6148914691236517205));
    C17_CHECK_EQ(g_ru64_3(UINT64_MAX), 0u);
    C17_CHECK_EQ(g_du64_7(UINT64_MAX), UINT64_C(2635249153387078802));
    C17_CHECK_EQ(g_ru64_7(UINT64_MAX), 1u);
    C17_CHECK_EQ(g_du64_10(UINT64_MAX), UINT64_C(1844674407370955161));
    C17_CHECK_EQ(g_ru64_10(UINT64_MAX), 5u);

    C17_CHECK_EQ(g_du32_big(UINT32_MAX), 1u);
    C17_CHECK_EQ(g_ru32_big(UINT32_MAX), UINT32_C(0x7FFFFFFE));
    C17_CHECK_EQ(g_du32_big(UINT32_C(0x80000000)), 0u);
    C17_CHECK_EQ(g_ru32_big(UINT32_C(0x80000000)), UINT32_C(0x80000000));

    /* 4. Narrow signed INT8_MIN / -1 and INT16_MIN / -1 (integer promotion -> valid!) */
    C17_CHECK_EQ(g_di8((int8_t)-128, (int8_t)-1), 128);
    C17_CHECK_EQ(g_di16((int16_t)-32768, (int16_t)-1), 32768);

    C17_END();
}
