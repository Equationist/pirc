// OPT_LEVEL: -O2
// EXPECT_RET: 0
// EXPECT_LIR_FORBIDS: select.ptr
target "aarch64-apple-darwin"

weak extern @weak_tls = tls i32

export func @no_speculate_tls(%cond: i8, %fallback: ptr) -> ptr cc "c" [nounwind] {
.entry:
    br %cond, .take, .skip
.take:
    %p = ptr_add.i64 @weak_tls, 4
    jmp .join(%p)
.skip:
    jmp .join(%fallback)
.join(%res: ptr):
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %s = alloca.ptr 8, align 8
    %r = call @no_speculate_tls(0, %s)
    %eq = cmp_eq.ptr %r, %s
    %ok = select.i32 %eq, 0, 1
    ret %ok
}
