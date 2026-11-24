// MODE: HIR
// EXPECT_RET: 58
target "aarch64-apple-darwin"

type @Point = { i32, i32 }
sig @TransformSig = (retbuf(@Point), byval(@Point), i32) -> void cc "c" [nounwind]
sig @ScoreSig     = (byval(@Point)) -> i32 cc "c" [nounwind]

export func @scale_point(%out: retbuf(@Point), %in: byval(@Point), %k: i32) -> void cc "c" [nounwind] {
.entry:
    %ix = field_ptr @Point, %in, 0
    %iy = field_ptr @Point, %in, 1
    %x = load.i32 %ix, align 4
    %y = load.i32 %iy, align 4
    %sx = mul.i32 %x, %k
    %sy = mul.i32 %y, %k
    %ox = field_ptr @Point, %out, 0
    %oy = field_ptr @Point, %out, 1
    store.i32 %sx, %ox, align 4
    store.i32 %sy, %oy, align 4
    ret
}

export func @point_score(%p: byval(@Point)) -> i32 cc "c" [nounwind] {
.entry:
    %px = field_ptr @Point, %p, 0
    %py = field_ptr @Point, %p, 1
    %x = load.i32 %px, align 4
    %y = load.i32 %py, align 4
    // 2*x + y
    %x2 = mul.i32 %x, 2
    %s = add.i32 %x2, %y
    ret %s
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %p = alloca @Point
    %q = alloca @Point
    %px = field_ptr @Point, %p, 0
    %py = field_ptr @Point, %p, 1
    store.i32 4, %px, align 4
    store.i32 10, %py, align 4

    %fn_tr = const.ptr @scale_point
    %fn_sc = const.ptr @point_score

    // scale (4, 10) by 3 -> (12, 30); then score = 2*12 + 30 = 54; plus orig x=4 -> 58
    call_indirect @TransformSig, %fn_tr(%q, %p, 3)
    %sc = call_indirect @ScoreSig, %fn_sc(%q)
    %ox = load.i32 %px, align 4
    %res = add.i32 %sc, %ox
    ret %res
}
