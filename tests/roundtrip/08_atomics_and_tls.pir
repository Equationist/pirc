target "x86_64-unknown-linux-gnu"

@g_lock = global i32 = 0, align 4
@t_tid  = tls i64 = 0, align 8

export func @cas_inc(%p: ptr) -> i32 {
.entry:
    %cur = atomic_load.i32 %p [acquire]
    jmp .retry(%cur)

.retry(%exp: i32):
    %next = add.i32 %exp, 1
    %old, %ok = atomic_cmpxchg.i32 %p, %exp, %next [acq_rel, acquire]
    br %ok, .done(%old), .retry(%old)

.done(%prev: i32):
    %added = atomic_rmw.add.i32 %p, 2 [seq_cst]
    atomic_store.i32 %added, %p [release]
    fence [seq_cst]
    ret %prev
}
