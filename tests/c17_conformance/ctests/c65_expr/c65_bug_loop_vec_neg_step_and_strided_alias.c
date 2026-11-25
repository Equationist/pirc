/*
 * C17 Conformance Test — Class C (§6.5 / Loop Vectorizer & Alias Safety)
 * Adversarial loop-vectorizer regressions:
 *   1) Negative-start signed i32 IV loop: for (int i = -16; i < 16; i++).
 *   2) Reverse-iteration loop: for (int i = n - 1; i >= 0; i--).
 *   3) Loop-carried dependency (prefix sum a[i] = a[i - 1] + b[i]) MUST NOT
 *      be mis-vectorized as independent lanes.
 *   4) Overlapping forward shift (a[i] = a[i + 1] + 1) vs overlapping backward
 *      shift (a[i] = a[i - 1] + 1) on the same array.
 */
#include "c17_test.h"
#include <stdint.h>

static void neg_start_fill(int32_t *base_mid, const int32_t *src_mid) {
    for (int i = -16; i < 16; i++) {
        base_mid[i] = src_mid[i] * 3 + i;
    }
}

static void reverse_scale(int32_t *dst, const int32_t *src, int n) {
    for (int i = n - 1; i >= 0; i--) {
        dst[i] = src[i] * 2 - 5;
    }
}

static void prefix_sum_dep(int32_t *a, const int32_t *b, int n) {
    for (int i = 1; i < n; i++) {
        a[i] = a[i - 1] + b[i];
    }
}

static void overlap_forward_shift(int32_t *a, int n) {
    for (int i = 0; i < n - 1; i++) {
        a[i] = a[i + 1] + 1;
    }
}

int main(void) {
    C17_BEGIN("c65_bug_loop_vec_neg_step_and_strided_alias",
              "Negative-start IV, reverse loop, loop-carried dep, overlapping shift");

    /* 1. Negative-start signed i32 IV */
    int32_t src[32], dst[32];
    for (int i = 0; i < 32; i++) src[i] = i - 10;
    neg_start_fill(&dst[16], &src[16]);
    for (int i = -16; i < 16; i++) {
        C17_CHECK(dst[16 + i] == src[16 + i] * 3 + i);
    }

    /* 2. Reverse-iteration loop */
    int32_t rdst[32];
    reverse_scale(rdst, src, c17_opaque_int(32));
    for (int i = 0; i < 32; i++) {
        C17_CHECK(rdst[i] == src[i] * 2 - 5);
    }

    /* 3. Loop-carried dependency (prefix sum) */
    int32_t pa[32], pb[32];
    pa[0] = 5;
    for (int i = 0; i < 32; i++) pb[i] = i + 1;
    prefix_sum_dep(pa, pb, c17_opaque_int(32));
    int32_t acc = 5;
    C17_CHECK(pa[0] == 5);
    for (int i = 1; i < 32; i++) {
        acc += i + 1;
        C17_CHECK(pa[i] == acc);
    }

    /* 4. Overlapping forward shift */
    int32_t sh[20];
    for (int i = 0; i < 20; i++) sh[i] = i * 10;
    overlap_forward_shift(sh, c17_opaque_int(20));
    for (int i = 0; i < 19; i++) {
        C17_CHECK(sh[i] == (i + 1) * 10 + 1);
    }
    C17_CHECK(sh[19] == 190);

    C17_END();
}
