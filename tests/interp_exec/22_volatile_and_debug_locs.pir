// MODE: LIR
// EXPECT_RET: 33
target "x86_64-unknown-linux-gnu"

file 0 "tests/interp_exec/22_volatile_and_debug_locs.pir"

weak func @helper_ro(%p: ptr readonly, %q: ptr noalias) -> i32 [nounwind] {
.entry:
    loc 0, 8, 5
    %v = load.i32 %p, align 4
    %w = load.i32 %q, align 4 [volatile]
    %s = add.i32 %v, %w
    ret %s
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    loc 0, 16, 5
    %s1 = alloca.ptr 4, align 4
    %s2 = alloca.ptr 4, align 4
    store.i32 21, %s1, align 4
    store.i32 12, %s2, align 4 [volatile]
    loc 0, 21, 5
    %res = call @helper_ro(%s1, %s2)
    ret %res
}
