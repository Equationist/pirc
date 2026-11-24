// MODE: HIR
// EXPECT_ERR: alignment 3 is not a non-zero power of two
target "x86_64-unknown-linux-gnu"

func @bad(%p: ptr) -> i32 {
.entry:
    %v = load.i32 %p, align 3
    ret %v
}
