// MODE: HIR
// EXPECT_ERR: br condition must have type i8, got i32
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    br %x, .t, .f
.t:
    ret 1
.f:
    ret 0
}
