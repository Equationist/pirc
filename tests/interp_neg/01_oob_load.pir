// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %p = alloca.ptr 4, align 4
    store.i32 1, %p, align 4
    %q = ptr_add.i64 %p, 4
    %v = load.i32 %q, align 4
    ret %v
}
