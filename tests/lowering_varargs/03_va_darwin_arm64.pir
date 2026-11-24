// MODE: HIR
// EXPECT_LIR_CONTAINS: alloca.ptr 8, align 8
// EXPECT_LIR_CONTAINS: va_start
// EXPECT_SIG: @vprintf (ptr, ptr) -> i32
// EXPECT_LIR_FORBIDS: va_list
// EXPECT_LIR_FORBIDS: va_arg
// EXPECT_LIR_FORBIDS: va_copy
// EXPECT_LIR_FORBIDS: va_end
target "aarch64-apple-darwin"

// On Darwin ARM64, va_list is an 8-byte char * pointer and all variadic
// arguments are passed strictly on the stack (no register-save area).
type @Pair = { i32, i32 }

extern func @vprintf(%fmt: ptr, %ap: byval(va_list)) -> i32 cc "c" [nounwind]

export func @darwin_varargs(%fmt: ptr, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap  = alloca va_list
    %ap2 = alloca va_list
    va_start %ap
    va_copy %ap2, %ap
    call @vprintf(%fmt, %ap2)
    va_end %ap2

    %a = va_arg.i32 %ap
    %b = va_arg.i64 %ap
    %c = va_arg.f64 %ap
    %pair = alloca @Pair
    va_arg_buf @Pair, %pair, %ap
    va_end %ap

    %px = field_ptr @Pair, %pair, 0
    %x = load.i32 %px, align 4
    %b32 = trunc.i32 %b
    %ci = fptosi.i32 %c [sat]
    %s1 = add.i32 %a, %b32
    %s2 = add.i32 %s1, %ci
    %res = add.i32 %s2, %x
    ret %res
}
