// EXPECT_EXIT: 132
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %r = div_s.i32 10, 0 [trap]
    ret %r
}
