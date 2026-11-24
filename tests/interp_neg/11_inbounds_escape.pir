// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %p = alloca.ptr 8, align 8
    %q = ptr_add.i64 %p, 9 [inbounds]
    ret 0
}
