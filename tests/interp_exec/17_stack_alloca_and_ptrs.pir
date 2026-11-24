// EXPECT_OUT: 99 1 99
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %u %d\n", 0 }

export func @main() -> i32 {
.entry:
    %arr = alloca.ptr 16, align 8
    %slot = alloca.ptr 8, align 8
    %p2 = ptr_add.i64 %arr, 8 [inbounds]
    store.i32 99, %p2, align 4
    store.ptr %p2, %slot, align 8
    %loaded_p = load.ptr %slot, align 8
    %v1 = load.i32 %loaded_p, align 4

    %end = ptr_add.i64 %arr, 16 [inbounds]
    %lt  = cmp_lt_u.ptr %p2, %end
    %lti = zext.i32 %lt

    %as_int = ptrtoint.i64 %loaded_p
    %back   = inttoptr.ptr %as_int
    %v2 = load.i32 %back, align 4

    %f = addr @fmt
    call @printf(%f, %v1, %lti, %v2)
    ret 0
}
