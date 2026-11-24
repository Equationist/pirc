target "x86_64-unknown-linux-gnu"

export func @lir_only(%p: ptr, %a: i64, %b: i64) -> i64 {
.entry:
    %slot = alloca.ptr 16, align 8
    %lo, %hi = mul_wide_u.i64 %a, %b
    %s, %c   = add_carry.i64 %lo, %hi, 0
    %p1      = ptr_add.i64 %slot, 8 [inbounds]
    store.i64 %s, %p1, align 8
    %loaded  = load.i64 %p1, align 8
    %q       = ptr_add.i64 %p, 0 [wrap]
    %iv      = ptrtoint.i64 %q
    %r       = xor.i64 %loaded, %iv
    ret %r
}
