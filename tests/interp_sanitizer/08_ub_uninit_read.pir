// MODE: LIR
// EXPECT_SANITIZER_ERR: uninitialized
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 4, align 4
    %v = load.i32 %buf, align 4
    ret %v
}
