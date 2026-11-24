// MODE: HIR
// EXPECT_ERR: cannot branch to entry block '.entry'
target "x86_64-unknown-linux-gnu"

func @bad() -> void {
.entry:
    jmp .entry
}
