// MODE: HIR
// EXPECT_ERR: va_start is only valid inside a variadic function
target "x86_64-unknown-linux-gnu"

func @bad(%x: i32) -> void {
.entry:
    %ap = alloca va_list
    va_start %ap
    va_end %ap
    ret
}
