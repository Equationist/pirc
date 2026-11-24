target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }
sig @BinOp = (i32, i32) -> i32 cc "c"

extern func @make_point(%out: retbuf(@Point), %x: i32, %y: i32) -> void cc "c"
extern func @point_area(%p: byval(@Point)) -> i32 cc "c"

export func @"_Z7run_abii"(%cb: ptr readonly, %k: i32) -> i32 cc "c" {
.entry:
    %p = alloca @Point
    call @make_point(%p, %k, 10)
    %a = call @point_area(%p)
    %r = call_indirect @BinOp, %cb(%a, %k)
    ret %r
}
