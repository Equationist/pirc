// MODE: LIR
// EXPECT_TRAP
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = const.i32 2147483647
    %b = add.i32 %a, 1 [trap_s]
    ret %b
}
