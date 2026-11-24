// EXPECT_KIND: trap
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %r = mul.i32 100000, 100000 [trap_s]
    ret %r
}
