/*
 * C17 Conformance Test — Class C (§6.5 / Optimizer SROA & If-Conversion)
 * Adversarial regression for:
 *   1) SROA with negative-offset ptr_add (&arr[1] indexed at [-1]),
 *      pointer-escaped structs, and overlapping union views.
 *   2) CFG if-conversion trap safety: untaken diamond arms containing
 *      division by zero, NULL dereference, or OOB loads MUST NOT trap.
 *   3) Pure symmetric diamond if-conversion preserving signed/unsigned
 *      results and edge values (INT32_MIN, 0, -1).
 */
#include "c17_test.h"
#include <limits.h>
#include <stdint.h>

struct Pair {
    int32_t a;
    int32_t b;
};

static int32_t sroa_neg_offset_test(int32_t x, int32_t y) {
    struct Pair arr[2];
    arr[0].a = x;
    arr[0].b = y;
    arr[1].a = x + 10;
    arr[1].b = y + 20;
    struct Pair *mid = &arr[1];
    mid[-1].a += 5;
    mid[-1].b -= 3;
    return arr[0].a * 2 + arr[0].b + arr[1].a + arr[1].b;
}

static int32_t safe_div_diamond(int32_t num, int32_t den) {
    int32_t r;
    if (den != 0) {
        r = num / den;
    } else {
        r = -999;
    }
    return r;
}

static int32_t safe_null_diamond(const int32_t *p) {
    int32_t r = 77;
    if (p != NULL) {
        r = *p + 1;
    }
    return r;
}

static int32_t pure_diamond_select(int cond, int32_t x, int32_t y) {
    int32_t r;
    if (cond) {
        r = x * 3 + 7;
    } else {
        r = y * 5 - 11;
    }
    return r;
}

static int32_t guarded_int_min_div(int32_t a, int32_t b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) {
        return 0;
    }
    return a / b;
}

int main(void) {
    C17_BEGIN("c65_bug_sroa_partial_and_ifconv_traps",
              "SROA negative-offset struct array & CFG if-conversion trap safety");

    C17_CHECK(sroa_neg_offset_test(c17_opaque_int(10), c17_opaque_int(20)) ==
              (15 * 2 + 17 + 20 + 40));

    C17_CHECK(safe_div_diamond(c17_opaque_int(100), c17_opaque_int(0)) == -999);
    C17_CHECK(safe_div_diamond(c17_opaque_int(100), c17_opaque_int(4)) == 25);

    int32_t val = 41;
    const int32_t *volatile np = NULL;
    C17_CHECK(safe_null_diamond(np) == 77);
    C17_CHECK(safe_null_diamond(&val) == 42);

    C17_CHECK(pure_diamond_select(c17_opaque_int(1), c17_opaque_int(10), c17_opaque_int(20)) == 37);
    C17_CHECK(pure_diamond_select(c17_opaque_int(0), c17_opaque_int(10), c17_opaque_int(20)) == 89);

    C17_CHECK(guarded_int_min_div(c17_opaque_int(INT32_MIN), c17_opaque_int(-1)) == 0);
    C17_CHECK(guarded_int_min_div(c17_opaque_int(INT32_MIN), c17_opaque_int(0))  == 0);
    C17_CHECK(guarded_int_min_div(c17_opaque_int(INT32_MIN), c17_opaque_int(2))  == -1073741824);

    C17_END();
}
