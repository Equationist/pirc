// MODE: HIR
// EXPECT_RET: 39
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Build 2^64 - 1 via zext from u64 max
    %u64_max = const.i64 -1
    %a = zext.i128 %u64_max
    // Cross 64-bit carry boundary: (2^64 - 1) + 10 = 2^64 + 9
    %b = add.i128 %a, 10 [wrap]
    // Store and reload i128 from stack
    %slot = alloca i128, align 16
    store.i128 %b, %slot, align 16
    %b_reloaded = load.i128 %slot, align 16

    // Check low 64 bits == 9 and high 64 bits == 1
    %lo1 = trunc.i64 %b_reloaded
    %hi1_128 = shr.i128 %b_reloaded, 64 [mask]
    %hi1 = trunc.i64 %hi1_128
    %k1 = cmp_eq.i64 %lo1, 9
    %k2 = cmp_eq.i64 %hi1, 1

    // Cross 64-bit borrow boundary: (2^64 + 9) - 20 = 2^64 - 11
    %c = sub.i128 %b_reloaded, 20 [wrap]
    %lo2 = trunc.i64 %c
    %hi2_128 = shr.i128 %c, 64 [mask]
    %hi2 = trunc.i64 %hi2_128
    %k3 = cmp_eq.i64 %lo2, -11
    %k4 = cmp_eq.i64 %hi2, 0

    // 128-bit wrap-around: (-1) + 40 == 39
    %neg1 = sext.i128 %u64_max
    %w = add.i128 %neg1, 40 [wrap]
    %w32 = trunc.i32 %w

    %m1 = and.i8 %k1, %k2
    %m2 = and.i8 %k3, %k4
    %ok = and.i8 %m1, %m2
    br %ok, .done(%w32), .fail

.done(%res: i32):
    ret %res

.fail:
    trap
}
