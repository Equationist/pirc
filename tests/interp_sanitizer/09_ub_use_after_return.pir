// MODE: LIR
// EXPECT_SANITIZER_ERR: use_after_free
target "x86_64-unknown-linux-gnu"

func @dangling() -> ptr [nounwind] {
.entry:
    %local = alloca.ptr 4, align 4
    store.i32 99, %local, align 4
    ret %local
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %p = call @dangling()
    %v = load.i32 %p, align 4
    ret %v
}
