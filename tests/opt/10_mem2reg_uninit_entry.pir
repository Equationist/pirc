// OPT_LEVEL: -O2
// EXPECT_RET: 0
// EXPECT_LIR_CONTAINS: ret %x
// EXPECT_LIR_FORBIDS: alloca
// Regression test: mem2reg promoting an uninitialized scalar alloca in .entry
// must not shift the terminator off .entry or invalidate fn->insts pointers.

target "aarch64-apple-darwin"

export func @uninit_entry_alloca(%x: i32) -> i32 cc "c" {
.entry:
    %a = alloca i32, align 4
    %b = alloca i64, align 8
    store.i32 %x, %a, align 4
    %u = load.i64 %b, align 8
    %v = load.i32 %a, align 4
    %t = trunc.i32 %u
    %r = add.i32 %v, %t [wrap]
    ret %r
}

export func @main() -> i32 cc "c" {
.entry:
    %a = alloca i32, align 4
    store.i32 42, %a, align 4
    %v = load.i32 %a, align 4
    %ok = cmp_eq.i32 %v, 42
    %res = select.i32 %ok, 0, 1
    ret %res
}
