// MODE: HIR
// EXPECT_LIR_CONTAINS: alloca.ptr 8, align 8
// EXPECT_LIR_CONTAINS: va_start
// EXPECT_SIG: @vprintf (ptr, ptr) -> i32
// EXPECT_LIR_FORBIDS: va_list
// EXPECT_LIR_FORBIDS: va_arg
// EXPECT_LIR_FORBIDS: va_copy
// EXPECT_LIR_FORBIDS: va_end
target "x86_64-pc-windows-msvc"

extern func @vprintf(%fmt: ptr, %ap: byval(va_list)) -> i32 cc "c" [nounwind]

export func @win64_varargs(%fmt: ptr, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    call @vprintf(%fmt, %ap)
    %a = va_arg.i32 %ap
    %b = va_arg.f64 %ap
    va_end %ap
    %bi = fptosi.i32 %b [sat]
    %res = add.i32 %a, %bi
    ret %res
}
