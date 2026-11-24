// MODE: LIR
// EXPECT_SANITIZER_ERR: out_of_bounds
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 8, align 4
    store.i32 1, %buf, align 4
    %oob = ptr_add.i64 %buf, 8 [wrap]
    %v = load.i32 %oob, align 4
    ret %v
}
