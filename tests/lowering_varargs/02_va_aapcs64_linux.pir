// MODE: HIR
// EXPECT_LIR_CONTAINS: alloca.ptr 32, align 8
// EXPECT_LIR_CONTAINS: va_start
// EXPECT_LIR_FORBIDS: va_list
// EXPECT_LIR_FORBIDS: va_arg
// EXPECT_LIR_FORBIDS: va_copy
// EXPECT_LIR_FORBIDS: va_end
target "aarch64-unknown-linux-gnu"

type @Pair = { i32, i32 }

extern func @vprintf(%fmt: ptr, %ap: byval(va_list)) -> i32 cc "c" [nounwind]

export func @aapcs64_varargs(%fmt: ptr, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap  = alloca va_list
    %ap2 = alloca va_list
    va_start %ap
    va_copy %ap2, %ap
    call @vprintf(%fmt, %ap2)
    va_end %ap2

    %a = va_arg.i32 %ap
    %b = va_arg.f64 %ap
    %pair = alloca @Pair
    va_arg_buf @Pair, %pair, %ap
    va_end %ap

    %bi = fptosi.i32 %b [sat]
    %res = add.i32 %a, %bi
    ret %res
}
