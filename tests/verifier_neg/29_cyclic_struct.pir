// MODE: HIR
// EXPECT_ERR: recursive/cyclic aggregate type '@Node'
target "x86_64-unknown-linux-gnu"

type @Node = { i32, @Node }

func @bad() -> void {
.entry:
    ret
}
