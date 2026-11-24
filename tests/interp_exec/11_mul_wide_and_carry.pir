// EXPECT_OUT: 1 0 1 0 9 0
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%llu %llu %u %llu %llu %u\n", 0 }

export func @main() -> i32 {
.entry:
    %max = const.i64 0xffffffffffffffff
    %lo, %hi = mul_wide_u.i64 %max, %max
    %sum, %cout = add_carry.i64 %max, 1, 0
    %dif, %bout = sub_borrow.i64 10, 0, 1
    %c32 = zext.i32 %cout
    %b32 = zext.i32 %bout
    %hi_plus = add.i64 %hi, 2
    %f = addr @fmt
    call @printf(%f, %lo, %sum, %c32, %hi_plus, %dif, %b32)
    ret 0
}
