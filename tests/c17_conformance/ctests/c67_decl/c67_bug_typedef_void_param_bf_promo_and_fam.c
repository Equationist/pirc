/*
 * C17 Conformance Test — Class C (§6.7.6.3 / §6.3.1.1 / §6.7.2.1 / §6.7.5 / §6.7.9)
 * Adversarial regressions for:
 *   1) typedef void V; int f(V) — empty parameter list via void typedef (§6.7.6.3p10).
 *   2) Bitfield integer promotions: unsigned : 31 promotes to int, unsigned : 32
 *      promotes to unsigned int (§6.3.1.1p2; GCC PR39059 / Clang PR11728).
 *   3) Flexible array member offsetof vs sizeof with tail padding (§6.7.2.1p18).
 *   4) Tentative/extern incomplete array completed later in the same TU.
 *   5) Function pointer compatibility ignoring top-level param qualifiers (§6.7.6.3p15).
 *   6) _Alignas(0) has no effect (§6.7.5p6).
 *   7) Auto union initialized via smaller/non-first member zero-fills remaining bytes.
 */
#include "c17_test.h"
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

typedef void VoidParam;
static int zero_param_via_typedef(VoidParam) {
    return 77;
}

struct BfPromo {
    unsigned a31 : 31;
    unsigned b32 : 32;
    unsigned short s15 : 15;
    unsigned short s16 : 16;
};

struct FamTailPad {
    int32_t  a;
    char     b;
    char     data[];
};

extern int g_deferred_arr[];
static int *get_deferred_arr(void) { return g_deferred_arr; }
int g_deferred_arr[] = { 11, 22, 33, 44 };

static int qualified_params_fn(const int a, volatile int b) {
    return a * 3 + b;
}

_Alignas(0) static int g_align0_var = 123;

union SmallBig {
    uint64_t big;
    uint8_t  small;
};

static uint64_t pollute_stack(uint64_t seed) {
    volatile uint64_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = seed ^ ((uint64_t)i * UINT64_C(0x9e3779b97f4a7c15));
    return buf[7];
}

static uint64_t make_union_small(uint8_t v) {
    union SmallBig u = { .small = v };
    return u.big;
}

int main(void) {
    C17_BEGIN("c67_bug_typedef_void_param_bf_promo_and_fam",
              "void typedef param, bitfield promo, FAM offsetof/sizeof, union zero-tail");

    /* 1. typedef void parameter */
    int (*zfp)(VoidParam) = zero_param_via_typedef;
    int (*zfp2)(void) = zfp;
    C17_CHECK(zero_param_via_typedef() == 77);
    C17_CHECK(zfp2() == 77);

    /* 2. Bitfield integer promotions */
    struct BfPromo bf = { 0u, 0u, 0u, 0u };
    C17_CHECK((bf.a31 - 1) < 0);
    C17_CHECK((bf.b32 - 1) > 0);
    C17_CHECK((bf.s15 - 1) < 0);
    C17_CHECK((bf.s16 - 1) < 0);
    C17_CHECK(_Generic(bf.a31 + 0, int: 1, unsigned: 2, default: 3) == 1);
    C17_CHECK(_Generic(bf.b32 + 0, int: 1, unsigned: 2, default: 3) == 2);
    C17_CHECK(_Generic(bf.s16 + 0, int: 1, unsigned: 2, default: 3) == 1);

    /* 3. FAM offsetof vs sizeof with tail padding */
    C17_CHECK(offsetof(struct FamTailPad, data) == 5);
    C17_CHECK(sizeof(struct FamTailPad) == 8);
    C17_CHECK(sizeof(struct FamTailPad) > offsetof(struct FamTailPad, data));

    /* 4. extern incomplete array completed later */
    C17_CHECK(sizeof(g_deferred_arr) == 4 * sizeof(int));
    C17_CHECK(get_deferred_arr()[0] == 11);
    C17_CHECK(get_deferred_arr()[3] == 44);

    /* 5. Function pointer parameter top-level qualifier compatibility */
    int (*qfp)(int, int) = qualified_params_fn;
    C17_CHECK(qfp(10, 5) == 35);

    /* 6. _Alignas(0) */
    _Alignas(0) int local_align0 = 456;
    C17_CHECK(g_align0_var == 123);
    C17_CHECK(local_align0 == 456);
    C17_CHECK(((uintptr_t)&g_align0_var % _Alignof(int)) == 0u);
    C17_CHECK(((uintptr_t)&local_align0 % _Alignof(int)) == 0u);

    /* 7. Union zero-tail auto initialization */
    (void)pollute_stack(UINT64_C(0xdeadbeefcafebabe));
    C17_CHECK(make_union_small(0x5au) == UINT64_C(0x5a));
    (void)pollute_stack(UINT64_C(0xffffffffffffffff));
    C17_CHECK(make_union_small(0x00u) == UINT64_C(0));

    C17_END();
}
