target "x86_64-unknown-linux-gnu"

export func @all_atomics(%p: ptr, %pp: ptr, %v: i64, %np: ptr) -> ptr {
.entry:
    %a0 = atomic_rmw.add.i64 %p, %v [relaxed]
    %a1 = atomic_rmw.sub.i64 %p, %a0 [acquire]
    %a2 = atomic_rmw.and.i64 %p, %a1 [release]
    %a3 = atomic_rmw.or.i64 %p, %a2 [acq_rel]
    %a4 = atomic_rmw.xor.i64 %p, %a3 [seq_cst]
    %a5 = atomic_rmw.xchg.i64 %p, %a4 [seq_cst]
    atomic_store.i64 %a5, %p [relaxed]
    %ld = atomic_load.ptr %pp [seq_cst]
    %old, %ok = atomic_cmpxchg.ptr %pp, %ld, %np [seq_cst, relaxed] [weak]
    fence [acquire]
    fence [release]
    fence [acq_rel]
    fence [seq_cst]
    %res = select.ptr %ok, %np, %old
    ret %res
}
