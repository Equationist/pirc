// MODE: LIR
// EXPECT_RET: 84
target "x86_64-unknown-linux-gnu"

// Exercises exact LIR primitives emitted by LegalizeI128:
// mul_wide_u, mul_wide_s, add_carry, sub_borrow
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1) Unsigned wide mul: (2^63) * 4 = 2^65 -> lo=0, hi=2
    %big = const.i64 -9223372036854775808
    %lo1, %hi1 = mul_wide_u.i64 %big, 4
    %c1 = cmp_eq.i64 %lo1, 0
    %c2 = cmp_eq.i64 %hi1, 2

    // 2) Signed wide mul: (-3) * 7 = -21 -> lo=-21, hi=-1
    %lo2, %hi2 = mul_wide_s.i64 -3, 7
    %c3 = cmp_eq.i64 %lo2, -21
    %c4 = cmp_eq.i64 %hi2, -1

    // 3) 128-bit add via add_carry: (hi=1, lo=0xFFFFFFFFFFFFFFFF) + (hi=2, lo=5) = (hi=4, lo=4)
    %alo = const.i64 -1
    %sum_lo, %cout = add_carry.i64 %alo, 5, 0
    %sum_hi, %cout2 = add_carry.i64 1, 2, %cout
    %c5 = cmp_eq.i64 %sum_lo, 4
    %c6 = cmp_eq.i8 %cout, 1
    %c7 = cmp_eq.i64 %sum_hi, 4
    %c8 = cmp_eq.i8 %cout2, 0

    // 4) 128-bit sub via sub_borrow: (hi=5, lo=2) - (hi=1, lo=10) = (hi=3, lo=-8)
    %dif_lo, %bout = sub_borrow.i64 2, 10, 0
    %dif_hi, %bout2 = sub_borrow.i64 5, 1, %bout
    %c9 = cmp_eq.i64 %dif_lo, -8
    %c10 = cmp_eq.i8 %bout, 1
    %c11 = cmp_eq.i64 %dif_hi, 3
    %c12 = cmp_eq.i8 %bout2, 0

    %m1 = and.i8 %c1, %c2
    %m2 = and.i8 %c3, %c4
    %m3 = and.i8 %c5, %c6
    %m4 = and.i8 %c7, %c8
    %m5 = and.i8 %c9, %c10
    %m6 = and.i8 %c11, %c12
    %n1 = and.i8 %m1, %m2
    %n2 = and.i8 %m3, %m4
    %n3 = and.i8 %m5, %m6
    %p1 = and.i8 %n1, %n2
    %ok = and.i8 %p1, %n3
    br %ok, .done, .fail

.done:
    ret 84

.fail:
    trap
}
