// MODE: HIR
// EXPECT_ERR: duplicate block label '.dup'
target "x86_64-unknown-linux-gnu"

func @bad() -> void {
.entry:
    jmp .dup
.dup:
    ret
.dup:
    ret
}
