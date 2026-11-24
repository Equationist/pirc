// MODE: HIR
// EXPECT_ERR: retbuf function must return void
target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }

func @bad(%out: retbuf(@Point)) -> i32 cc "c" {
.entry:
    ret 0
}
