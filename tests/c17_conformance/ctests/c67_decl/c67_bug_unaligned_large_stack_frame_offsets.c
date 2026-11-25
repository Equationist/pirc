/*
 * C17 Conformance Test — Class C (§6.7.5 / §6.5.2.3)
 * Backend frame/offset stress regression (ARM64/x64):
 *   Exercises unaligned/odd stack-frame offsets across >255B (ldur limit),
 *   >4095B (add/sub imm12 limit), and >32760B (scaled ldr/str imm12*8 limit)
 *   for i8/i16/i32/i64/f32/f64 and _Alignas(16) objects, combined with
 *   nested calls preserving high callee-saved GP + FP register pressure.
 */
#include "c17_test.h"
#include <stdalign.h>
#include <stdint.h>

struct OddFrame300 {
    char     pad[261]; /* > 255, odd offset */
    int8_t   s8;
    int16_t  s16;
    int32_t  s32;
    int64_t  s64;
    float    f32;
    double   f64;
};

struct HugeFrame {
    char     pad1[4109];  /* > 4095, unaligned */
    int64_t  mid64;
    double   midd;
    char     pad2[32803]; /* total > 36KB (> 32760 scaled imm12*8) */
    int8_t   hi8;
    int16_t  hi16;
    int32_t  hi32;
    int64_t  hi64;
    float    hif32;
    double   hif64;
};

static int64_t leaf_mix(int64_t a, int64_t b, double c, double d) {
    return a * 3 - b + (int64_t)(c * 4.0) + (int64_t)(d * 8.0);
}

static int64_t big_frame_compute(int64_t seed) {
    _Alignas(16) int64_t aligned_anchor[2];
    volatile struct OddFrame300 odd;
    volatile struct HugeFrame huge;

    /* Live GP + FP values across nested calls while huge stack frame is active */
    int64_t g0 = seed + 1, g1 = seed + 2, g2 = seed + 3, g3 = seed + 4;
    int64_t g4 = seed + 5, g5 = seed + 6, g6 = seed + 7, g7 = seed + 8;
    double  f0 = (double)seed * 0.25;
    double  f1 = (double)seed * 0.50;
    double  f2 = (double)seed * 0.75;
    double  f3 = (double)seed * 1.25;

    aligned_anchor[0] = seed ^ INT64_C(0x12345);
    aligned_anchor[1] = ((uintptr_t)&aligned_anchor[0] & 15u) == 0u ? 1 : 0;

    odd.s8  = (int8_t)(seed - 20);
    odd.s16 = (int16_t)(seed * 11);
    odd.s32 = (int32_t)(seed * 101);
    odd.s64 = seed * 1001;
    odd.f32 = (float)seed + 0.25f;
    odd.f64 = (double)seed + 0.125;

    huge.mid64 = seed * 7;
    huge.midd  = (double)seed * 1.5;
    huge.hi8   = (int8_t)(seed + 9);
    huge.hi16  = (int16_t)(seed * 13);
    huge.hi32  = (int32_t)(seed * 103);
    huge.hi64  = seed * 1003;
    huge.hif32 = (float)seed + 0.5f;
    huge.hif64 = (double)seed + 0.375;

    int64_t c1 = leaf_mix(odd.s64, huge.mid64, f0, f1);
    int64_t c2 = leaf_mix(huge.hi64, odd.s32, f2, f3);

    int64_t sum = aligned_anchor[0] + aligned_anchor[1]
                + odd.s8 + odd.s16 + odd.s32 + odd.s64
                + (int64_t)(odd.f32 * 4.0f) + (int64_t)(odd.f64 * 8.0)
                + huge.mid64 + (int64_t)(huge.midd * 2.0)
                + huge.hi8 + huge.hi16 + huge.hi32 + huge.hi64
                + (int64_t)(huge.hif32 * 2.0f) + (int64_t)(huge.hif64 * 8.0)
                + c1 + c2
                + g0 + g1 + g2 + g3 + g4 + g5 + g6 + g7
                + (int64_t)((f0 + f1 + f2 + f3) * 4.0);
    return sum;
}

int main(void) {
    C17_BEGIN("c67_bug_unaligned_large_stack_frame_offsets",
              "Unaligned >255B/>4KB/>36KB stack frame offsets and high reg pressure");

    int64_t s = c17_opaque_ll(12);
    int64_t got = big_frame_compute(s);

    /* Independent scalar reference calculation for seed = 12 */
    int64_t a0 = 12 ^ INT64_C(0x12345), a1 = 1;
    int64_t c1 = leaf_mix(12 * 1001, 12 * 7, 3.0, 6.0);
    int64_t c2 = leaf_mix(12 * 1003, 12 * 101, 9.0, 15.0);
    int64_t want = a0 + a1
                 + (-8) + (12 * 11) + (12 * 101) + (12 * 1001)
                 + 49 + 97
                 + (12 * 7) + 36
                 + 21 + (12 * 13) + (12 * 103) + (12 * 1003)
                 + 25 + 99
                 + c1 + c2
                 + (13 + 14 + 15 + 16 + 17 + 18 + 19 + 20)
                 + 132;
    C17_CHECK(got == want);

    C17_END();
}
