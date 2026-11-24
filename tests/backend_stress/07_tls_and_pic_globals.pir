// MODE: LIR
// EXPECT_RET: 54
target "aarch64-apple-darwin"

// Exercises Mach-O PIC globals (@PAGE/@PAGEOFF), weak definitions, and Darwin TLS
// (__DATA,__thread_vars TLV thunk clobbering caller-saved regs while live values exist).
@g_data    = data align 1 { 5, 9, 13, 0 }
@g_bss     = global i64 = zeroinit, align 8
@g_ptr_off = data align 8 { ptr @g_data + 1 }
@t_slot    = tls i64 = 13, align 8

weak func @weak_helper(%x: i32) -> i32 cc "c" [nounwind] {
.entry:
    %r = mul.i32 %x, 2
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Hold 6 live GP values across Darwin TLS TLV thunk materialization & call
    %k0 = const.i64 1
    %k1 = add.i64 %k0, 1
    %k2 = add.i64 %k1, 1
    %k3 = add.i64 %k2, 1
    %k4 = add.i64 %k3, 1
    %k5 = add.i64 %k4, 1

    // Access Darwin TLS variable (@TLVPAGE/@TLVPAGEOFF + ldr x8,[x0]; blr x8)
    %tp = addr @t_slot
    %tv = load.i64 %tp, align 8
    %tv1 = add.i64 %tv, 7
    store.i64 %tv1, %tp, align 8
    %tv2 = load.i64 %tp, align 8

    // Check bss == 0 and rodata + offset
    %bp = addr @g_bss
    %bv = load.i64 %bp, align 8
    %gp = addr @g_data
    %b0 = load.i8 %gp, align 1
    %pp = addr @g_ptr_off
    %p1 = load.ptr %pp, align 8
    %b1 = load.i8 %p1, align 1
    %b1_ok = cmp_eq.i8 %b1, 9
    %bv_ok = cmp_eq.i64 %bv, 0
    %ok = and.i8 %b1_ok, %bv_ok
    br %ok, .finish(%k0, %k1, %k2, %k3, %k4, %k5, %tv2, %b0), .fail

.finish(%a0: i64, %a1: i64, %a2: i64, %a3: i64, %a4: i64, %a5: i64, %tls_v: i64, %d0: i8):
    %w = call @weak_helper(4)
    %w64 = sext.i64 %w
    %d64 = zext.i64 %d0
    %s0 = add.i64 %a0, %a1
    %s1 = add.i64 %a2, %a3
    %s2 = add.i64 %a4, %a5
    %ks = add.i64 %s0, %s1
    %ks2 = add.i64 %ks, %s2
    // ks2=21, tls_v=20, d64=5, w64=8 -> 21 + 20 + 5 + 8 = 54
    %t1 = add.i64 %ks2, %tls_v
    %t2 = add.i64 %t1, %d64
    %tot = add.i64 %t2, %w64
    %res = trunc.i32 %tot
    ret %res

.fail:
    trap
}
