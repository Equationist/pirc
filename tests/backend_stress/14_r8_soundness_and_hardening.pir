// Round 8 soundness & hardening regression tests:
// - Union global with ptr relocation when fields[0] is non-ptr
// - Pad-only wrapper aggregate globals & deep pad DAG memoization
// - Direct f32 literal parsing (no double-rounding on decimal midpoint)
// - ARM64 status-register-named symbol (@nzcv) quoting
// - memset with dynamic byte and constant len 0
// - Aggregate `memory` ABI attribute (SysV x86-64 MEMORY class for sz<=2)
// - 256-bit vector C-ABI param/return lowering
// - Self-tail-call elimination & inlining with ABI metadata clearing
// - LICM TLS address non-speculation & [readonly] call safety
target "aarch64-apple-darwin"

type @Pad1 = { pad 4 }
type @Pad2 = { @Pad1, @Pad1 }
type @Pad3 = { @Pad2, @Pad2 }
type @Pad4 = { @Pad3, @Pad3 }
type @Pad5 = { @Pad4, @Pad4 }
type @Pad6 = { @Pad5, @Pad5 }
type @Pad7 = { @Pad6, @Pad6 }
type @Pad8 = { @Pad7, @Pad7 }
type @Pad9 = { @Pad8, @Pad8 }
type @Pad10 = { @Pad9, @Pad9 }
type @Pad11 = { @Pad10, @Pad10 }
type @Pad12 = { @Pad11, @Pad11 }

type @UnionReloc = union { i64, ptr }
type @Mem2 = [2 x i8], align 2, memory

@target_val = global i64 = 123456789, align 8
@u_reloc = global @UnionReloc = { ptr @target_val }, align 8
@pad_g = global @Pad12 = zeroinit, align 1
@tls_g = tls i64 = 77, align 8
@nzcv = global i64 = 42, align 8

func @ro_helper(%p: ptr) -> i64 cc "c" readonly {
.entry:
    %v = load.i64 %p, align 8
    ret %v
}

func @mem2_roundtrip(%out: retbuf(@Mem2), %in: byval(@Mem2)) -> void cc "c" [nounwind] {
.entry:
    %b0 = load.i8 %in, align 1
    %p1 = ptr_add.i64 %in, 1
    %b1 = load.i8 %p1, align 1
    %n0 = add.i8 %b0, 1 [wrap]
    %n1 = add.i8 %b1, 2 [wrap]
    store.i8 %n0, %out, align 1
    %o1 = ptr_add.i64 %out, 1
    store.i8 %n1, %o1, align 1
    ret
}

func @vec256_add_sum(%a: v8i32, %b: v8i32) -> i32 cc "c" [nounwind] {
.entry:
    %s = add.v8i32 %a, %b [wrap]
    %e0 = extract.v8i32 %s, 0
    %e7 = extract.v8i32 %s, 7
    %sum = add.i32 %e0, %e7 [wrap]
    ret %sum
}

func @tco_sum(%n: i64, %acc: i64) -> i64 cc "c" [nounwind] {
.entry:
    %done = cmp_le_s.i64 %n, 0
    br %done, .ret_acc, .step
.ret_acc:
    ret %acc
.step:
    %n1 = sub.i64 %n, 1 [wrap]
    %acc1 = add.i64 %acc, %n [wrap]
    %r = call @tco_sum(%n1, %acc1)
    ret %r
}

func @licm_tls_and_ro(%n: i64, %buf: ptr) -> i64 cc "c" [nounwind] {
.entry:
    jmp .loop(0, 0)
.loop(%i: i64, %acc: i64):
    %cond = cmp_lt_s.i64 %i, %n
    br %cond, .body, .exit
.body:
    %tp = addr @tls_g
    %tv = load.i64 %tp, align 8
    store.i64 %i, %buf, align 8
    %rv = call @ro_helper(%buf)
    %t1 = add.i64 %acc, %tv [wrap]
    %t2 = add.i64 %t1, %rv [wrap]
    %ni = add.i64 %i, 1 [wrap]
    jmp .loop(%ni, %t2)
.exit:
    ret %acc
}

func @f32_exact_parse() -> i32 cc "c" [nounwind] {
.entry:
    // 7.038531e-26 is a classic double-rounding midpoint for IEEE-754 binary32:
    // direct strtof -> 0x15a4a126, double->float -> 0x15a4a125
    %f = const.f32 7.038531e-26
    %bits = bitcast.i32 %f
    %ok = cmp_eq.i32 %bits, 363110694
    %r = zext.i32 %ok
    ret %r
}

export func @main() -> i32 cc "c" {
.entry:
    // 1. Union relocation flattening
    %up = addr @u_reloc
    %ptr_val = load.ptr %up, align 8
    %tv = load.i64 %ptr_val, align 8
    %ok1 = cmp_eq.i64 %tv, 123456789

    // 2. ARM64 status-register-named symbol @nzcv
    %np = addr @nzcv
    %nv = load.i64 %np, align 8
    %ok2 = cmp_eq.i64 %nv, 42

    // 3. memset len 0 with dynamic fill byte must be a no-op
    %slot = alloca i64, align 8
    store.i64 999, %slot, align 8
    %dyn_b = trunc.i8 %nv
    memset %slot, %dyn_b, 0, align 8
    %sv = load.i64 %slot, align 8
    %ok3 = cmp_eq.i64 %sv, 999

    // 4. `memory`-tagged 2-byte aggregate byval/retbuf
    %m_in = alloca @Mem2, align 2
    %m_out = alloca @Mem2, align 2
    store.i8 10, %m_in, align 1
    %mi1 = ptr_add.i64 %m_in, 1
    store.i8 20, %mi1, align 1
    call @mem2_roundtrip(%m_out, %m_in)
    %mo0 = load.i8 %m_out, align 1
    %mo1p = ptr_add.i64 %m_out, 1
    %mo1 = load.i8 %mo1p, align 1
    %ok4a = cmp_eq.i8 %mo0, 11
    %ok4b = cmp_eq.i8 %mo1, 22
    %ok4 = and.i8 %ok4a, %ok4b

    // 5. 256-bit vector C-ABI call (params passed via indirect/memory on 128-bit targets)
    %va = splat.v8i32 3
    %vb = splat.v8i32 4
    %vsum = call @vec256_add_sum(%va, %vb)
    %ok5 = cmp_eq.i32 %vsum, 14

    // 6. TCO + LICM TLS & readonly helper
    %ts = call @tco_sum(10, 0)
    %ok6 = cmp_eq.i64 %ts, 55
    %ls = call @licm_tls_and_ro(4, %slot)
    // 4*77 + (0+1+2+3) = 308 + 6 = 314
    %ok7 = cmp_eq.i64 %ls, 314

    // 7. Exact f32 parsing (no double rounding)
    %fok = call @f32_exact_parse()
    %ok8 = cmp_eq.i32 %fok, 1

    %a1 = and.i8 %ok1, %ok2
    %a2 = and.i8 %a1, %ok3
    %a3 = and.i8 %a2, %ok4
    %a4 = and.i8 %a3, %ok5
    %a5 = and.i8 %a4, %ok6
    %a6 = and.i8 %a5, %ok7
    %all = and.i8 %a6, %ok8
    %res = zext.i32 %all
    %retv = sub.i32 1, %res [wrap]
    ret %retv
}
