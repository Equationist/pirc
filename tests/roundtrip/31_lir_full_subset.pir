target "x86_64-unknown-linux-gnu"

sig @Callback = (ptr, i32) -> i64 cc "c"

@g_val = global i64 = 7, align 8
@t_val = tls ptr = 0, align 8

export func @lir_complete(%cb: ptr, %x: i32, %v: v4f32) -> i64 cc "c" nounwind {
.entry:
    %s   = alloca.ptr 32, align 16
    %p   = ptr_add.i64 %s, 16 [inbounds]
    store.v4f32 %v, %p, align 16
    %lv  = load.v4f32 %p, align 16
    %e   = extract.v4f32 %lv, 0
    %ei  = fptosi.i32 %e [sat]
    %res = call_indirect @Callback, %cb(%s, %ei) [nounwind]
    %gp  = addr @g_val
    %gv  = load.i64 %gp, align 8
    %tp  = addr @t_val
    store.ptr %s, %tp, align 8
    %tot = add.i64 %res, %gv
    ret %tot
}
