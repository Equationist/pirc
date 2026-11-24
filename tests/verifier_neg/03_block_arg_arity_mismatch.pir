// MODE: HIR
// EXPECT_ERR: block argument count mismatch for '.join' (expected 2, got 1)
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> i32 {
.entry:
    jmp .join(%x)
.join(%a: i32, %b: i32):
    %r = add.i32 %a, %b
    ret %r
}
