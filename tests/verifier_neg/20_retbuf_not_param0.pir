// MODE: HIR
// EXPECT_ERR: retbuf must be parameter 0
target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }

func @bad(%x: i32, %out: retbuf(@Point)) -> void cc "c" {
.entry:
    ret
}
