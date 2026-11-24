// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %sp = stacksave
    %p  = alloca_dyn 8, align 8
    store.i32 5, %p, align 4
    stackrestore %sp
    %v  = load.i32 %p, align 4
    ret %v
}
