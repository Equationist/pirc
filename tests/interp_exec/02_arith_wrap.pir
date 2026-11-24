// EXPECT_OUT: 0 255 -2147483648 0 7 2
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %u %d %d %u %u\n", 0 }

export func @main() -> i32 {
.entry:
    %a8 = const.i8 200
    %b8 = const.i8 56
    %w8 = add.i8 %a8, %b8 [wrap]
    %z0 = const.i8 0
    %s8 = sub.i8 %z0, 1 [wrap]
    %w8i = zext.i32 %w8
    %s8i = zext.i32 %s8

    %min = const.i32 -2147483648
    %neg1 = const.i32 -1
    %dw = div_s.i32 %min, %neg1 [wrap]
    %rw = rem_s.i32 %min, %neg1 [wrap]

    %u1 = const.i32 23
    %u2 = const.i32 3
    %du = div_u.i32 %u1, %u2
    %ru = rem_u.i32 %u1, %u2

    %f = addr @fmt
    call @printf(%f, %w8i, %s8i, %dw, %rw, %du, %ru)
    ret 0
}
