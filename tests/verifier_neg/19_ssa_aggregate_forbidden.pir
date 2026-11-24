// MODE: HIR
// EXPECT_ERR: type '@Point' is not valid in SSA
target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }

func @bad(%p: @Point) -> void {
.entry:
    ret
}
