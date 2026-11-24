// EXPECT_KIND: ub
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 {
.entry:
    %c = const.i8 2
    br %c, .t, .f
.t:
    ret 1
.f:
    ret 0
}
