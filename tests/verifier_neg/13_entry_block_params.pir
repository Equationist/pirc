// MODE: HIR
// EXPECT_ERR: entry block '.entry' must not declare block parameters
target "x86_64-unknown-linux-gnu"

func @bad() -> i32 {
.entry(%x: i32):
    ret %x
}
