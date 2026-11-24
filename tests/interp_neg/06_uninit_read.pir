// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %p = alloca.ptr 4, align 4
    %v = load.i32 %p, align 4
    ret %v
}
