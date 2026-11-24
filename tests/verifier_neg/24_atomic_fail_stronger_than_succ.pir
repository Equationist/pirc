// MODE: HIR
// EXPECT_ERR: invalid atomic_cmpxchg failure ordering
target "x86_64-unknown-linux-gnu"

func @bad(%p: ptr, %e: i32, %n: i32) -> i8 {
.entry:
    %old, %ok = atomic_cmpxchg.i32 %p, %e, %n [relaxed, seq_cst]
    ret %ok
}
