target "aarch64-apple-darwin"

type @Pair  = { i32, i32 }
type @Big24 = { i64, i64, i64 }

@fmt_out = data align 1 { "pir:%d:%.2f:%s", 0 }
@str_ok  = data align 1 { "ok", 0 }

extern func @vsnprintf(%buf: ptr, %cap: i64, %fmt: ptr, %ap: byval(va_list)) -> i32 cc "c" [nounwind]
extern func @snprintf(%buf: ptr, %cap: i64, %fmt: ptr, ...) -> i32 cc "c" [nounwind]
extern func @c_consume_varargs(%tag: i32, ...) -> i64 cc "c" [nounwind]

export func @pir_sum_varargs(%tag: i32, ...) -> i64 cc "c" [nounwind] {
.entry:
    %ap  = alloca va_list
    %ap2 = alloca va_list
    va_start %ap
    va_copy %ap2, %ap

    %a  = va_arg.i32 %ap
    %b  = va_arg.i64 %ap
    %c  = va_arg.f64 %ap
    %pr = alloca @Pair
    va_arg_buf @Pair, %pr, %ap
    va_end %ap

    // Verify va_copy reads the first variadic arg independently
    %a_copy = va_arg.i32 %ap2
    va_end %ap2

    %pxp = field_ptr @Pair, %pr, 0
    %pyp = field_ptr @Pair, %pr, 1
    %px  = load.i32 %pxp, align 4
    %py  = load.i32 %pyp, align 4

    %tag64 = sext.i64 %tag
    %a64   = sext.i64 %a
    %ac64  = sext.i64 %a_copy
    %c64   = fptosi.i64 %c [sat]
    %px64  = sext.i64 %px
    %py64  = sext.i64 %py

    %s1 = add.i64 %tag64, %a64
    %s2 = add.i64 %b, %c64
    %s3 = add.i64 %px64, %py64
    %s4 = add.i64 %s1, %s2
    %s5 = add.i64 %s3, %ac64
    %res = add.i64 %s4, %s5
    ret %res
}

export func @pir_format_via_vsnprintf(%buf: ptr, %cap: i64, %fmt: ptr, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %n = call @vsnprintf(%buf, %cap, %fmt, %ap)
    va_end %ap
    ret %n
}

export func @pir_drive_c_varargs(%out_buf: ptr, %cap: i64) -> i32 cc "c" [nounwind] {
.entry:
    // 1. Call host C snprintf with typed inline literals + string ptr
    %fmt = const.ptr @fmt_out
    %sok = const.ptr @str_ok
    %n = call @snprintf(%out_buf, %cap, %fmt, 42: i32, 3.25: f64, %sok)

    // 2. Call custom C varargs with scalar + Pair (8B) + Big24 (24B)
    %pr = alloca @Pair
    %px = field_ptr @Pair, %pr, 0
    %py = field_ptr @Pair, %pr, 1
    store.i32 10, %px, align 4
    store.i32 20, %py, align 4

    %bg = alloca @Big24
    %b0 = field_ptr @Big24, %bg, 0
    %b1 = field_ptr @Big24, %bg, 1
    %b2 = field_ptr @Big24, %bg, 2
    store.i64 100, %b0, align 8
    store.i64 200, %b1, align 8
    store.i64 300, %b2, align 8

    // Expected: 5 + 7 + 1000 + 12 + 10 + 20 + 100 + 200 + 300 = 1654
    %got = call @c_consume_varargs(5, 7: i32, 1000: i64, 12.0: f64, %pr: byval(@Pair), %bg: byval(@Big24))
    %c1 = cmp_eq.i64 %got, 1654
    %c2 = cmp_gt_s.i32 %n, 0
    %ok = and.i8 %c1, %c2
    %res = zext.i32 %ok
    ret %res
}
