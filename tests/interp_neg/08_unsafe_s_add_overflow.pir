// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %r = add.i32 2147483647, 1 [unsafe_s]
    ret %r
}
