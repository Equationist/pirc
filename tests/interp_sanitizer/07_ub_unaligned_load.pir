// MODE: LIR
// EXPECT_SANITIZER_ERR: alignment
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 8, align 4
    store.i64 0, %buf, align 4
    %mis = ptr_add.i64 %buf, 1 [inbounds]
    %v = load.i32 %mis, align 4
    ret %v
}
