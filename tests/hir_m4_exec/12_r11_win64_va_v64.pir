// MODE: HIR
// EXPECT_RET: 0
// Round 11 regression: Win64 variadic 64-bit vector args (v2i32) must be passed indirectly.
target "x86_64-pc-windows-msvc"

func @callee(%n: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %v = va_arg.v2i32 %ap
    va_end %ap
    %e0 = extract.v2i32 %v, 0
    %e1 = extract.v2i32 %v, 1
    %s = add.i32 %e0, %e1 [wrap]
    ret %s
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %v0 = splat.v2i32 10
    %v1 = insert.v2i32 %v0, 32, 1
    %r = call @callee(1, %v1: v2i32)
    %ok = cmp_eq.i32 %r, 42
    %rc = select.i32 %ok, 0, %r
    ret %rc
}
