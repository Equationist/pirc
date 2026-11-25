/*
 * C17 §6.5 & §6.9.1: High register pressure (20+ simultaneously live GP and FP
 * variables across function calls and loop back-edges) + many stack arguments
 * (12 int + 12 double + 2 structs).
 */
#include "c17_test.h"

struct TagPair {
    int a;
    int b;
};

static long long clobber_call(long long x, double y) {
    return x + (long long)(y * 2.0);
}

static long long many_args_fn(
    long long i0, long long i1, long long i2, long long i3,
    long long i4, long long i5, long long i6, long long i7,
    long long i8, long long i9, long long i10, long long i11,
    double d0, double d1, double d2, double d3,
    double d4, double d5, double d6, double d7,
    double d8, double d9, double d10, double d11,
    struct TagPair p1, struct TagPair p2
) {
    long long isum = i0 + i1 + i2 + i3 + i4 + i5 + i6 + i7 + i8 + i9 + i10 + i11;
    double dsum = d0 + d1 + d2 + d3 + d4 + d5 + d6 + d7 + d8 + d9 + d10 + d11;
    return isum + (long long)dsum + p1.a + p1.b + p2.a + p2.b;
}

static long long live_pressure_test(long long seed) {
    long long a0 = seed + 1,  a1 = seed + 2,  a2 = seed + 3,  a3 = seed + 4;
    long long a4 = seed + 5,  a5 = seed + 6,  a6 = seed + 7,  a7 = seed + 8;
    long long a8 = seed + 9,  a9 = seed + 10, a10 = seed + 11, a11 = seed + 12;
    long long a12 = seed + 13, a13 = seed + 14, a14 = seed + 15, a15 = seed + 16;

    double f0 = 0.5, f1 = 1.5, f2 = 2.5, f3 = 3.5;
    double f4 = 4.5, f5 = 5.5, f6 = 6.5, f7 = 7.5;
    double f8 = 8.5, f9 = 9.5, f10 = 10.5, f11 = 11.5;

    long long acc = 0;
    for (int iter = 0; iter < 3; iter++) {
        acc += clobber_call(a0 + a15, f0 + f11);
        /* Rotate a subset of live variables to stress regalloc phi cycles */
        long long t = a0; a0 = a1; a1 = a2; a2 = a3; a3 = t;
        double ft = f0; f0 = f1; f1 = f2; f2 = ft;
    }

    long long all_i = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 +
                      a8 + a9 + a10 + a11 + a12 + a13 + a14 + a15;
    double all_f = f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9 + f10 + f11;
    return acc + all_i + (long long)all_f;
}

int main(void) {
    C17_BEGIN("c65_deep_regalloc_stress", "28 live GP/FP values across calls + 26 mixed stack arguments");

    long long r_many = many_args_fn(
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0, 11.0, 12.0,
        (struct TagPair){ 100, 200 },
        (struct TagPair){ 300, 400 }
    );
    /* 78 + 78 + 1000 = 1156 */
    C17_CHECK_EQ(r_many, 1156);

    /* seed=0:
     * all_i = 1+..+16 = 136; all_f = 0.5+1.5+..+11.5 = 72.0
     * iter 0: clobber(1+16, 0.5+11.5) = 17 + 24 = 41
     * iter 1: clobber(2+16, 1.5+11.5) = 18 + 26 = 44
     * iter 2: clobber(3+16, 2.5+11.5) = 19 + 28 = 47
     * acc = 132 -> total = 132 + 136 + 72 = 340 */
    C17_CHECK_EQ(live_pressure_test(c17_opaque_ll(0)), 340);

    C17_END();
}
