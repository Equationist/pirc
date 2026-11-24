// MODE: HIR
// EXPECT_RET: 61
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. 64x64 -> 128 multiply crossing 2^64:
    // (2^40 + 3) * (2^40 + 5) = 2^80 + 8*2^40 + 15
    %one = const.i128 1
    %p40 = shl.i128 %one, 40 [mask]
    %x = add.i128 %p40, 3
    %y = add.i128 %p40, 5
    %prod = mul.i128 %x, %y [wrap]

    // Expected hi64 = 2^16 + (8 >> 24) = 65536; lo64 = (8 << 40) + 15 = 0x000008000000000F
    %lo = trunc.i64 %prod
    %hi_128 = shr.i128 %prod, 64 [mask]
    %hi = trunc.i64 %hi_128
    %c1 = cmp_eq.i64 %hi, 65536
    %c2 = cmp_eq.i64 %lo, 0x000008000000000F

    // 2. Shifts across <64, ==64, >64, and dynamic register count + [mask] (132 mod 128 == 4)
    %s_lt = shl.i128 %one, 12 [mask]
    %s_gt = shl.i128 %one, 70 [mask]
    %s_back = shr.i128 %s_gt, 70 [mask]
    %dyn_sh = const.i128 132
    %s_mask = shl.i128 3, %dyn_sh [mask]

    // 3. Arithmetic right shift of negative i128: (-2^70) >> 68 == -4
    %neg_big = sub.i128 0, %s_gt
    %sar_v = sar.i128 %neg_big, 68 [mask]
    %sar32 = trunc.i32 %sar_v

    %c3 = cmp_eq.i128 %s_lt, 4096
    %c4 = cmp_eq.i128 %s_back, 1
    %c5 = cmp_eq.i128 %s_mask, 48
    %c6 = cmp_eq.i32 %sar32, -4

    %m1 = and.i8 %c1, %c2
    %m2 = and.i8 %c3, %c4
    %m3 = and.i8 %c5, %c6
    %n1 = and.i8 %m1, %m2
    %ok = and.i8 %n1, %m3
    br %ok, .done(%s_mask), .fail

.done(%v: i128):
    // 48 + 13 = 61
    %v32 = trunc.i32 %v
    %res = add.i32 %v32, 13
    ret %res

.fail:
    trap
}
