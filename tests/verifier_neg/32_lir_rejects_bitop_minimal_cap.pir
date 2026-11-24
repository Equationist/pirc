// MODE: LIR
// CAPS: minimal
// EXPECT_ERR: LIR target does not support bitops
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    %r = clz.i32 %x
    ret %r
}
