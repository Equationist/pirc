/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ISO/IEC 9899:2018 (C17) §6.2.8 / §6.5.2.2 — Regression tests for:
 *   1. ARM64 shrink-wrapping deleting 32-bit zero-extending `mov wN, wN`.
 *   2. Over-aligned (_Alignas(32/64)) byval struct copies (PIR_OP_ALLOCA_PTR
 *      dynamic realignment on both ARM64 and x86-64).
 *   3. x86-64 VLA caller (has_alloca_dyn) passing _Alignas(32/64) outgoing
 *      stack arguments with dynamic RSP save/realign/restore.
 */
#include <stdalign.h>
#include <stdint.h>
#include "c17_test.h"

/* --- Part 1: ARM64 shrink-wrap 32-bit zero-extension preservation --- */
static uint64_t sink_impl(uint64_t x) { return x * 5ULL + 11ULL; }
typedef uint64_t (*sink_fn_t)(uint64_t);
static volatile sink_fn_t g_sink = sink_impl;

static uint64_t sw_leaf_zext(uint32_t x, int flag) {
    if (flag == 0) return (uint64_t)x;
    uint64_t r = g_sink((uint64_t)(uint32_t)flag);
    return r + (uint64_t)x;
}

static int sw_prefix_zext(uint32_t x, int flag) {
    uint64_t y = (uint64_t)x;
    if (y == 42ULL) return 1;
    uint64_t r = g_sink(y + (uint64_t)(uint32_t)flag);
    return (int)(r & 0xFFFFu);
}

static uint64_t sw_loop_zext(uint32_t n, uint32_t init) {
    uint64_t acc = (uint64_t)init;
    while (n > 0u) {
        acc += g_sink((uint64_t)n);
        n--;
    }
    return acc;
}

static uint64_t (*volatile g_leaf)(uint32_t, int) = sw_leaf_zext;
static int      (*volatile g_pref)(uint32_t, int) = sw_prefix_zext;
static uint64_t (*volatile g_loop)(uint32_t, uint32_t) = sw_loop_zext;

static uint64_t call_leaf(uint64_t big, int flag) { return g_leaf((uint32_t)big, flag); }
static int      call_pref(uint64_t big, int flag) { return g_pref((uint32_t)big, flag); }
static uint64_t call_loop(uint64_t bn, uint64_t bi) { return g_loop((uint32_t)bn, (uint32_t)bi); }

/* --- Part 2 & 3: Over-aligned byval + VLA outgoing stack args --- */
struct A32 { _Alignas(32) uint64_t a[4]; };
struct A64 { _Alignas(64) uint64_t a[8]; };

static uintptr_t recv_a32(int a, int b, int c, int d, int e, int f, int g, struct A32 s) {
    volatile struct A32 *p = &s;
    return (((uintptr_t)p) % 32u) + (s.a[0] == 11 && s.a[3] == 44 ? 0u : 100u)
         + (uintptr_t)((a + b + c + d + e + f + g) & 0);
}

static uintptr_t recv_a64(int a, int b, int c, int d, int e, int f, int g,
                          int h, int i, struct A64 s) {
    volatile struct A64 *p = &s;
    return (((uintptr_t)p) % 64u) + (s.a[0] == 101 && s.a[7] == 808 ? 0u : 100u)
         + (uintptr_t)((a + b + c + d + e + f + g + h + i) & 0);
}

static uintptr_t (*volatile g_fn32)(int,int,int,int,int,int,int,struct A32) = recv_a32;
static uintptr_t (*volatile g_fn64)(int,int,int,int,int,int,int,int,int,struct A64) = recv_a64;

static uintptr_t recurse_direct_a64(int depth) {
    if (depth > 0) {
        volatile char pad[16];
        pad[0] = (char)depth;
        return recurse_direct_a64(depth - 1) + (uintptr_t)(pad[0] & 0);
    }
    struct A64 s = {{101, 202, 303, 404, 505, 606, 707, 808}};
    return recv_a64(1, 2, 3, 4, 5, 6, 7, 8, 9, s);
}

static uintptr_t recurse_indirect_a64(int depth) {
    if (depth > 0) {
        volatile char pad[16];
        pad[0] = (char)depth;
        return recurse_indirect_a64(depth - 1) + (uintptr_t)(pad[0] & 0);
    }
    struct A64 s = {{101, 202, 303, 404, 505, 606, 707, 808}};
    return g_fn64(1, 2, 3, 4, 5, 6, 7, 8, 9, s);
}

static uintptr_t call_a32_with_vla(int n, int depth) {
    if (depth > 0) {
        volatile char pad[16];
        pad[0] = (char)depth;
        return call_a32_with_vla(n, depth - 1) + (uintptr_t)(pad[0] & 0);
    }
    volatile char vla[n];
    vla[0] = 1;
    vla[n - 1] = 2;
    struct A32 s = {{11, 22, 33, 44}};
    return g_fn32(1, 2, 3, 4, 5, 6, 7, s) + (uintptr_t)((vla[0] + vla[n - 1]) & 0);
}

static uintptr_t call_a64_with_vla(int n, int depth) {
    if (depth > 0) {
        volatile char pad[16];
        pad[0] = (char)depth;
        return call_a64_with_vla(n, depth - 1) + (uintptr_t)(pad[0] & 0);
    }
    volatile char vla[n];
    vla[0] = 3;
    vla[n - 1] = 4;
    struct A64 s = {{101, 202, 303, 404, 505, 606, 707, 808}};
    return g_fn64(1, 2, 3, 4, 5, 6, 7, 8, 9, s) + (uintptr_t)((vla[0] + vla[n - 1]) & 0);
}

int main(void) {
    C17_BEGIN("c65_bug_arm64_shrink_wrap_zext32_and_alignas_vla",
              "ARM64 shrink-wrap zext32 preservation, _Alignas(32/64) byval & VLA stack args");

    volatile uint64_t dirty42 = 0xDEADBEEF0000002AULL;
    volatile uint64_t dirty0  = 0xCAFEBABE00000000ULL;

    C17_CHECK_EQ(call_leaf(dirty42, 0), 0x2aULL);
    C17_CHECK_EQ(call_leaf(dirty42, 1), 0x3aULL);
    C17_CHECK_EQ(call_pref(dirty42, 0), 1);
    C17_CHECK_EQ(call_pref(dirty42 + 1ULL, 3), 241);
    C17_CHECK_EQ(call_loop(dirty0, dirty42), 0x2aULL);
    C17_CHECK_EQ(call_loop(dirty0 + 3ULL, dirty42), 0x69ULL);

    int bad_direct = 0, bad_indirect = 0, bad_vla32 = 0, bad_vla64 = 0;
    for (int d = 0; d < 8; ++d) {
        if (recurse_direct_a64(d) != 0) bad_direct++;
        if (recurse_indirect_a64(d) != 0) bad_indirect++;
        if (call_a32_with_vla(16 + d * 16, d) != 0) bad_vla32++;
        if (call_a64_with_vla(16 + d * 16, d) != 0) bad_vla64++;
    }
    C17_CHECK_EQ(bad_direct, 0);
    C17_CHECK_EQ(bad_indirect, 0);
    C17_CHECK_EQ(bad_vla32, 0);
    C17_CHECK_EQ(bad_vla64, 0);

    C17_END();
}
