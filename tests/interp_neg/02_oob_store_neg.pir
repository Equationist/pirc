// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %p = alloca.ptr 8, align 4
    %q = ptr_add.i64 %p, -4
    store.i32 1, %q, align 4
    ret 0
}
