// EXPECT_OUT: 3628800
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%lld\n", 0 }

func @fact(%n: i64) -> i64 {
.entry:
    %base = cmp_le_s.i64 %n, 1
    br %base, .one, .rec

.one:
    ret 1

.rec:
    %n1  = sub.i64 %n, 1
    %sub = call @fact(%n1)
    %res = mul.i64 %n, %sub [trap_s]
    ret %res
}

export func @main() -> i32 {
.entry:
    %r = call @fact(10)
    %f = addr @fmt
    call @printf(%f, %r)
    ret 0
}
