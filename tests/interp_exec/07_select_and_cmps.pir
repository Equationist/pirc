// EXPECT_OUT: 100 200 1 0
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %d %d %d\n", 0 }

export func @main() -> i32 {
.entry:
    %neg = const.i32 -5
    %pos = const.i32 5
    %cs  = cmp_lt_s.i32 %neg, %pos
    %cu  = cmp_lt_u.i32 %neg, %pos
    %s1  = select.i32 %cs, 100, 200
    %s2  = select.i32 %cu, 100, 200
    %i1  = zext.i32 %cs
    %i2  = zext.i32 %cu
    %f   = addr @fmt
    call @printf(%f, %s1, %s2, %i1, %i2)
    ret 0
}
