// MODE: HIR
// EXPECT_LIR_CONTAINS: alloca.ptr 24, align 8
// EXPECT_LIR_CONTAINS: va_start
// EXPECT_SIG: @vprintf (ptr, ptr) -> i32
// EXPECT_LIR_FORBIDS: va_list
// EXPECT_LIR_FORBIDS: va_arg
// EXPECT_LIR_FORBIDS: va_copy
// EXPECT_LIR_FORBIDS: va_end
target "x86_64-unknown-linux-gnu"

type @Pair = { i32, i32 }

extern func @vprintf(%fmt: ptr, %ap: byval(va_list)) -> i32 cc "c" [nounwind]

export func @sysv_varargs(%fmt: ptr, %count: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap  = alloca va_list
    %ap2 = alloca va_list
    va_start %ap
    va_copy %ap2, %ap
    call @vprintf(%fmt, %ap2)
    va_end %ap2

    %iv = va_arg.i32 %ap
    %fv = va_arg.f64 %ap
    %pair = alloca @Pair
    va_arg_buf @Pair, %pair, %ap
    va_end %ap

    %px = field_ptr @Pair, %pair, 0
    %x = load.i32 %px, align 4
    %fi = fptosi.i32 %fv [sat]
    %s1 = add.i32 %iv, %fi
    %res = add.i32 %s1, %x
    ret %res
}
