// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %r = sub.i32 0, 1 [unsafe_u]
    ret %r
}
