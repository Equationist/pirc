// MODE: HIR
// EXPECT_ERR: block argument 0 type mismatch for '.join' (expected i64, got i32)
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i64 {
.entry:
    jmp .join(%x)
.join(%a: i64):
    ret %a
}
