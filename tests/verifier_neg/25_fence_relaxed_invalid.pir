// MODE: HIR
// EXPECT_ERR: invalid memory ordering for fence
target "x86_64-unknown-linux-gnu"

func @bad() -> void {
.entry:
    fence [relaxed]
    ret
}
