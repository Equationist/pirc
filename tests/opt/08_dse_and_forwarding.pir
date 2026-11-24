// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_RET: 42
// EXPECT_LIR_FORBIDS: 1111
// EXPECT_LIR_CONTAINS: 2222
// EXPECT_LIR_CONTAINS: 3333
// EXPECT_LIR_CONTAINS: fence [seq_cst]
target "aarch64-apple-darwin"

// Dead store (1111) overwritten before read vanishes and store-to-load forwarding fires.
export func @dse_elim(%p: ptr noalias) -> i32 cc "c" [nounwind] {
.entry:
    store.i32 1111, %p, align 4
    store.i32 42, %p, align 4
    %v = load.i32 %p, align 4
    ret %v
}

// Volatile stores and stores separated by a fence/atomic barrier MUST survive DSE.
export func @dse_barriers(%p: ptr) -> i32 cc "c" [nounwind] {
.entry:
    store.i32 2222, %p, align 4 [volatile]
    store.i32 3333, %p, align 4
    fence [seq_cst]
    store.i32 7, %p, align 4
    %v = load.i32 %p, align 4 [volatile]
    ret %v
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %slot = alloca.ptr 4, align 4
    %a = call @dse_elim(%slot)
    ret %a
}
