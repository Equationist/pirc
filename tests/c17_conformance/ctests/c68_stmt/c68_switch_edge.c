/*
 * C99 §6.8.4.2: switch edge cases — 64-bit signed/unsigned values (no 32-bit
 * truncation!), default-before-case fallthrough, Duff's device, nested switch,
 * case inside if, small integer promotion, and empty switch.
 */
#include "c17_test.h"
#include <limits.h>
#include <stdint.h>

/* 64-bit unsigned switch: values differing ONLY in upper 32 bits! */
static int sw_u64(unsigned long long x) {
    switch (x) {
    case 0x0000000000000001ULL: return 1;
    case 0x0000000100000001ULL: return 2;
    case 0x0000000200000001ULL: return 3;
    case 0xFFFFFFFFFFFFFFFFULL: return 4;
    default:                    return 0;
    }
}

/* 64-bit signed switch with extreme values */
static int sw_s64(long long x) {
    switch (x) {
    case LLONG_MIN: return -100;
    case -1LL:      return -1;
    case 0LL:       return 0;
    case LLONG_MAX: return 100;
    default:        return 999;
    }
}

/* default placed BEFORE case labels and falling through into a case! */
static int sw_default_first(int x) {
    int acc = 0;
    switch (x) {
    default:
        acc += 100;
        /* intentional fallthrough into case 1 */
    case 1:
        acc += 10;
        break;
    case 2:
        acc += 20;
        break;
    }
    return acc;
}

/* Duff's device: case labels interleaved inside do-while loop! */
static void duff_copy(int *dst, const int *src, int count) {
    int n = (count + 3) / 4;
    switch (count % 4) {
    case 0: do { *dst++ = *src++;
    case 3:      *dst++ = *src++;
    case 2:      *dst++ = *src++;
    case 1:      *dst++ = *src++;
            } while (--n > 0);
    }
}

/* Case nested inside an if statement + nested switch */
static int sw_nested_and_if(int a, int b) {
    int r = 0;
    switch (a) {
    case 1:
        switch (b) {
        case 10: r = 110; break;
        default: r = 199; break;
        }
        r += 1; /* Inner break MUST NOT exit outer switch! */
        break;
    if (0) {
    case 2:
        r = 222;
    }
        break;
    default:
        r = -1;
        break;
    }
    return r;
}

int main(void) {
    C99_BEGIN("c68_switch_edge", "switch: 64-bit, default-first, Duff's device, nested, case-in-if");

    C99_CHECK_EQ(sw_u64(c99_opaque_ull(0x0000000000000001ULL)), 1);
    C99_CHECK_EQ(sw_u64(c99_opaque_ull(0x0000000100000001ULL)), 2);
    C99_CHECK_EQ(sw_u64(c99_opaque_ull(0x0000000200000001ULL)), 3);
    C99_CHECK_EQ(sw_u64(c99_opaque_ull(0xFFFFFFFFFFFFFFFFULL)), 4);
    C99_CHECK_EQ(sw_u64(c99_opaque_ull(0x0000000300000001ULL)), 0);

    C99_CHECK_EQ(sw_s64(c99_opaque_ll(LLONG_MIN)), -100);
    C99_CHECK_EQ(sw_s64(c99_opaque_ll(-1LL)), -1);
    C99_CHECK_EQ(sw_s64(c99_opaque_ll(LLONG_MAX)), 100);
    C99_CHECK_EQ(sw_s64(c99_opaque_ll(42LL)), 999);

    C99_CHECK_EQ(sw_default_first(c99_opaque_int(99)), 110);
    C99_CHECK_EQ(sw_default_first(c99_opaque_int(1)), 10);
    C99_CHECK_EQ(sw_default_first(c99_opaque_int(2)), 20);

    int src[10] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    int dst[10] = { 0 };
    duff_copy(dst, src, 10);
    int dsum = 0;
    for (int i = 0; i < 10; i++) {
        dsum += dst[i];
    }
    C99_CHECK_EQ(dsum, 55);
    C99_CHECK_EQ(dst[9], 10);

    C99_CHECK_EQ(sw_nested_and_if(1, 10), 111);
    C99_CHECK_EQ(sw_nested_and_if(1, 5), 200);
    C99_CHECK_EQ(sw_nested_and_if(2, 0), 222);

    /* Empty switch still evaluates controlling expression */
    int side = 0;
    switch (++side) { }
    C99_CHECK_EQ(side, 1);

    C99_END();
}
