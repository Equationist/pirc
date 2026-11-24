// MODE: LIR
// EXPECT_TRAP
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    trap
}
