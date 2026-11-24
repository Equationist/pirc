// MODE: LIR
// EXPECT_RET: 64
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %slot = alloca.ptr 4, align 4
    atomic_store.i32 10, %slot [release]
    %v0 = atomic_load.i32 %slot [acquire]
    %c0 = cmp_eq.i32 %v0, 10

    // RMW ops: 10 + 15 = 25; 25 - 5 = 20; 20 | 3 = 23; 23 & 30 = 22; 22 ^ 6 = 16; xchg -> 40
    %r1 = atomic_rmw.add.i32 %slot, 15 [seq_cst]
    %r2 = atomic_rmw.sub.i32 %slot, 5 [acq_rel]
    %r3 = atomic_rmw.or.i32 %slot, 3 [relaxed]
    %r4 = atomic_rmw.and.i32 %slot, 30 [relaxed]
    %r5 = atomic_rmw.xor.i32 %slot, 6 [relaxed]
    %r6 = atomic_rmw.xchg.i32 %slot, 40 [seq_cst]

    %k1 = cmp_eq.i32 %r1, 10
    %k2 = cmp_eq.i32 %r2, 25
    %k3 = cmp_eq.i32 %r3, 20
    %k4 = cmp_eq.i32 %r4, 23
    %k5 = cmp_eq.i32 %r5, 22
    %k6 = cmp_eq.i32 %r6, 16

    // Failing CAS: expects 99, sees 40 -> old=40, ok=0
    %old_f, %ok_f = atomic_cmpxchg.i32 %slot, 99, 100 [acq_rel, acquire]
    // Succeeding CAS: expects 40, sets 64 -> old=40, ok=1
    %old_s, %ok_s = atomic_cmpxchg.i32 %slot, 40, 64 [seq_cst, acquire]
    fence [seq_cst]

    %k7 = cmp_eq.i32 %old_f, 40
    %k8 = cmp_eq.i8 %ok_f, 0
    %k9 = cmp_eq.i32 %old_s, 40
    %k10 = cmp_eq.i8 %ok_s, 1

    %m1 = and.i8 %c0, %k1
    %m2 = and.i8 %k2, %k3
    %m3 = and.i8 %k4, %k5
    %m4 = and.i8 %k6, %k7
    %m5 = and.i8 %k8, %k9
    %n1 = and.i8 %m1, %m2
    %n2 = and.i8 %m3, %m4
    %n3 = and.i8 %m5, %k10
    %p1 = and.i8 %n1, %n2
    %ok = and.i8 %p1, %n3
    br %ok, .done, .fail

.done:
    %final = atomic_load.i32 %slot [seq_cst]
    ret %final

.fail:
    trap
}
