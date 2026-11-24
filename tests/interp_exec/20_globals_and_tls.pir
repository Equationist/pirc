// EXPECT_OUT: 47 77
target "x86_64-unknown-linux-gnu"

sig @Op = (i32) -> i32 cc "c"
extern func @printf(%fmt: ptr, ...) -> i32 cc "c"

@fmt    = data align 1 { "%d %d\n", 0 }
@vtable = data align 8 { ptr @add_five }
@g_num  = global i32 = 42, align 4
@t_val  = tls i32 = 77, align 4

export func @add_five(%x: i32) -> i32 cc "c" {
.entry:
    %r = add.i32 %x, 5
    ret %r
}

export func @main() -> i32 {
.entry:
    %vt = addr @vtable
    %fn = load.ptr %vt, align 8
    %gp = addr @g_num
    %gv = load.i32 %gp, align 4
    %res = call_indirect @Op, %fn(%gv)
    %tp = addr @t_val
    %tv = load.i32 %tp, align 4
    %f = addr @fmt
    call @printf(%f, %res, %tv)
    ret 0
}
