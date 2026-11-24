target "x86_64-unknown-linux-gnu"

type @Pair = { i32, i32 }

extern func @vprintf(%fmt: ptr, %ap: byval(va_list)) -> i32 cc "c"
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@fmt_str = data align 1 { "sum=%d
", 0 }

export func @log_and_sum(%fmt: ptr, %n: i32, ...) -> i32 cc "c" {
.entry:
    %ap  = alloca va_list
    %ap2 = alloca va_list
    va_start %ap
    va_copy %ap2, %ap
    call @vprintf(%fmt, %ap2)
    va_end %ap2

    %pair = alloca @Pair
    va_arg_buf @Pair, %pair, %ap
    %first = va_arg.i32 %ap
    va_end %ap

    %f = const.ptr @fmt_str
    call @printf(%f, %first, 42: i32, 3.14: f64, %pair: byval(@Pair))
    ret %first
}
