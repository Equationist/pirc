// MODE: HIR
// EXPECT_ERR: terminator appears in middle of block '.entry'
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    ret %x
    %y = add.i32 %x, 1
    ret %y
}
