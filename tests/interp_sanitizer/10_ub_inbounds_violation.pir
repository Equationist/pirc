// MODE: LIR
// EXPECT_SANITIZER_ERR: inbounds
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 8, align 4
    %bad = ptr_add.i64 %buf, 100 [inbounds]
    %v = ptrtoint.i32 %bad
    ret %v
}
