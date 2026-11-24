// MODE: HIR
// EXPECT_RET: 69
target "aarch64-apple-darwin"

// Tests exact M4 phase-order composition: LowerExceptions -> LowerCAbi -> LowerAggregates
type @Point = { i32, i32 }

@g_err = global i32 = 30, align 4

export func @checked_double(%out: retbuf(@Point), %in: byval(@Point)) -> void cc "c" {
.entry:
    %ix = field_ptr @Point, %in, 0
    %iy = field_ptr @Point, %in, 1
    %x = load.i32 %ix, align 4
    %y = load.i32 %iy, align 4
    %bad = cmp_lt_s.i32 %x, 0
    br %bad, .do_throw, .do_ok

.do_throw:
    %ep = const.ptr @g_err
    resume %ep

.do_ok:
    %dx = mul.i32 %x, 2
    %dy = mul.i32 %y, 2
    %ox = field_ptr @Point, %out, 0
    %oy = field_ptr @Point, %out, 1
    store.i32 %dx, %ox, align 4
    store.i32 %dy, %oy, align 4
    ret
}

export func @main() -> i32 cc "c" {
.entry:
    %in_ok = alloca @Point
    %out_ok = alloca @Point
    %p0 = field_ptr @Point, %in_ok, 0
    %p1 = field_ptr @Point, %in_ok, 1
    store.i32 9, %p0, align 4
    store.i32 10, %p1, align 4

    // 1. Succeeding invoke of C-ABI function with retbuf + byval: (9,10) -> (18,20)
    invoke @checked_double(%out_ok, %in_ok) then .after_ok unwind .bad_fail(exn)

.after_ok:
    %ox = field_ptr @Point, %out_ok, 0
    %oy = field_ptr @Point, %out_ok, 1
    %vx = load.i32 %ox, align 4
    %vy = load.i32 %oy, align 4
    %sum_ok = add.i32 %vx, %vy

    // 2. Throwing invoke of same C-ABI function (x = -1 -> throws payload 30)
    %in_bad = alloca @Point
    %out_bad = alloca @Point
    %bx = field_ptr @Point, %in_bad, 0
    %by = field_ptr @Point, %in_bad, 1
    store.i32 -1, %bx, align 4
    store.i32 5, %by, align 4
    invoke @checked_double(%out_bad, %in_bad) then .bad_ok unwind .caught(exn, %sum_ok)

.caught(%e: ptr, %s_prev: i32):
    %ev = load.i32 %e, align 4
    // sum_ok = 38, ev = 30 -> 38 + 30 + 1 = 69
    %t = add.i32 %s_prev, %ev
    %res = add.i32 %t, 1
    ret %res

.bad_fail(%u1: ptr):
    trap

.bad_ok:
    trap
}
