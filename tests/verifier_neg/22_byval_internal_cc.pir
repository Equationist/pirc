// MODE: HIR
// EXPECT_ERR: byval/retbuf requires cc "c"
target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }

func @bad(%p: byval(@Point)) -> void {
.entry:
    ret
}
