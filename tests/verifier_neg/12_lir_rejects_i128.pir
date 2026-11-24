// MODE: LIR
// EXPECT_ERR: LIR forbids 'i128' (must be legalized to i64 pairs)
target "x86_64-unknown-linux-gnu"

func @bad(%a: i128, %b: i128) -> i128 {
.entry:
    %r = add.i128 %a, %b
    ret %r
}
