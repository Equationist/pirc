// MODE: HIR
// EXPECT_ERR: block '.entry' does not end with a terminator
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    %y = add.i32 %x, 1
}
