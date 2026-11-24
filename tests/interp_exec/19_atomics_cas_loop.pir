// EXPECT_OUT: 10 0 1 15 1
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %u %u %d %u\n", 0 }
@cell = global i32 = 10, align 4

export func @main() -> i32 {
.entry:
    %p = addr @cell
    %v0 = atomic_load.i32 %p [acquire]
    %old1, %ok1 = atomic_cmpxchg.i32 %p, 99, 20 [acq_rel, acquire]
    %old2, %ok2 = atomic_cmpxchg.i32 %p, 10, 12 [acq_rel, acquire]
    %prev = atomic_rmw.add.i32 %p, 3 [seq_cst]
    fence [seq_cst]
    %fin = atomic_load.i32 %p [seq_cst]

    %pp = alloca.ptr 8, align 8
    atomic_store.ptr %p, %pp [release]
    %pold, %pok = atomic_cmpxchg.ptr %pp, %p, 0 [seq_cst, relaxed]

    %o1 = zext.i32 %ok1
    %o2 = zext.i32 %ok2
    %po = zext.i32 %pok
    %f = addr @fmt
    call @printf(%f, %v0, %o1, %o2, %fin, %po)
    ret 0
}
