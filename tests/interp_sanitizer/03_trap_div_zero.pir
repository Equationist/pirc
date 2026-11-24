// MODE: LIR
// EXPECT_TRAP
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = const.i32 10
    %b = const.i32 0
    %q = div_s.i32 %a, %b [trap]
    ret %q
}
