// MODE: HIR
// EXPECT_ERR: flag '[fast]' is only valid on floating-point operations
target "x86_64-unknown-linux-gnu"

func @bad(%a: i32, %b: i32) -> i32 {
.entry:
    %r = add.i32 %a, %b [fast]
    ret %r
}
