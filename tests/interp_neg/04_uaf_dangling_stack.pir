// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

func @leak_stack() -> ptr {
.entry:
    %p = alloca.ptr 4, align 4
    store.i32 99, %p, align 4
    ret %p
}

export func @main() -> i32 {
.entry:
    %dangling = call @leak_stack()
    %v = load.i32 %dangling, align 4
    ret %v
}
