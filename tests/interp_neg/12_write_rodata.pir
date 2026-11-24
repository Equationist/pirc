// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

@ro = data align 1 { "const", 0 }

export func @main() -> i32 {
.entry:
    %p = addr @ro
    store.i8 65, %p, align 1
    ret 0
}
