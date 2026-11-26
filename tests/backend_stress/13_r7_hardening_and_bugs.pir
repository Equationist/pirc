target "aarch64-apple-darwin"

// Round 7 PIR soundness & backend hardening regression suite:
// 1. Negative zero (-0.0) float/vector globals must NOT be folded to .zero/.bss
// 2. Braced vector { zeroinit 16 } global & O(1) pad-only large array global
// 3. GVN must NOT CSE inttoptr across distinct provenances or hoist ptr_add [inbounds]
// 4. simplify_cfg: if_convert_diamonds preserves -0.0 / NaN (no fast-math leakage);
//    thread_empty_jumps deep-copies args; self-TCO rejects byval
// 5. mem2reg / dce: unreachable preds, defs used by unreachable blocks, SROA alignment
// 6. loop_vec: negative i32 imm sign-extension & i64 shift-wrap guard
// 7. Large >4KB frame with high GP pressure (ARM64 x16/x17 scratch isolation)

type @Pair16 = { i64, i64 }
type @Pad16  = { pad 16 }
type @PadBig = [1024 x @Pad16]

@g_negz_f32 = data f32 = -0.0, align 4
@g_negz_f64 = data f64 = -0.0, align 8
@g_negz_vec = data v2f64 = { -0.0, 0.0 }, align 16
@g_zero_vec = data v4i32 = { zeroinit 16 }, align 16
@g_pad_big  = data @PadBig = zeroinit, align 16

// 1. Check -0.0 globals preserve their sign bit.
func @test_negzero_globals() -> i32 [nounwind] {
.entry:
    %pf32 = addr @g_negz_f32
    %f32v = load.f32 %pf32, align 4
    %b32  = bitcast.i32 %f32v
    %ok1  = cmp_eq.i32 %b32, 0x80000000

    %pf64 = addr @g_negz_f64
    %f64v = load.f64 %pf64, align 8
    %b64  = bitcast.i64 %f64v
    %ok2  = cmp_eq.i64 %b64, 0x8000000000000000

    %pvec = addr @g_negz_vec
    %l0   = load.i64 %pvec, align 8
    %p1   = ptr_add.i64 %pvec, 8
    %l1   = load.i64 %p1, align 8
    %ok3  = cmp_eq.i64 %l0, 0x8000000000000000
    %ok4  = cmp_eq.i64 %l1, 0

    %pzv  = addr @g_zero_vec
    %zv   = load.i64 %pzv, align 8
    %ok5  = cmp_eq.i64 %zv, 0

    %a1 = and.i8 %ok1, %ok2
    %a2 = and.i8 %ok3, %ok4
    %a3 = and.i8 %a1, %a2
    %a4 = and.i8 %a3, %ok5
    %r  = zext.i32 %a4
    ret %r
}

// 2. GVN must NOT CSE inttoptr of same integer across distinct allocas/provenances,
//    nor hoist ptr_add [inbounds] out of a guarded branch.
func @test_gvn_inttoptr_and_inbounds(%cond: i8) -> i32 [nounwind] {
.entry:
    %a = alloca i32, align 4
    %b = alloca i32, align 4
    store.i32 10, %a, align 4
    store.i32 20, %b, align 4
    %ia = ptrtoint.i64 %a
    %p1 = inttoptr.ptr %ia
    %p2 = inttoptr.ptr %ia
    store.i32 33, %p1, align 4
    %v  = load.i32 %p2, align 4
    %c  = cmp_ne.i8 %cond, 0
    br %c, .yes, .no
.yes:
    %q = ptr_add.i64 %b, 0 [inbounds]
    %vb = load.i32 %q, align 4
    %sum = add.i32 %v, %vb [wrap]
    jmp .done(%sum)
.no:
    jmp .done(%v)
.done(%res: i32):
    ret %res
}

// 3. if_convert_diamonds must preserve -0.0 (not leak [fast] flag).
func @test_diamond_negzero(%c: i8, %x: f64) -> i64 [nounwind] {
.entry:
    %b = cmp_ne.i8 %c, 0
    br %b, .t, .f
.t:
    %r1 = fadd.f64 %x, -0.0
    jmp .m(%r1)
.f:
    %r2 = fadd.f64 %x, 0.0
    jmp .m(%r2)
.m(%v: f64):
    %bits = bitcast.i64 %v
    ret %bits
}

// 4. Self-recursive byval must NOT be mis-tail-call-optimized into overwriting caller frame.
func @test_byval_rec(%s: byval(@Pair16), %n: i32) -> i64 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Pair16, %s, 0
    %p1 = field_ptr @Pair16, %s, 1
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %done = cmp_le_s.i32 %n, 0
    br %done, .base, .rec
.base:
    %ans = add.i64 %v0, %v1 [wrap]
    ret %ans
.rec:
    %tmp = alloca @Pair16, align 8
    %tp0 = field_ptr @Pair16, %tmp, 0
    %tp1 = field_ptr @Pair16, %tmp, 1
    %nv0 = add.i64 %v0, 1 [wrap]
    %nv1 = add.i64 %v1, 2 [wrap]
    store.i64 %nv0, %tp0, align 8
    store.i64 %nv1, %tp1, align 8
    %nn = sub.i32 %n, 1 [wrap]
    %r = call @test_byval_rec(%tmp, %nn)
    ret %r
}

// 5. Unreachable block referencing reachable def + unreachable predecessor to merge block.
func @test_unreach_ssa(%x: i32) -> i32 [nounwind] {
.entry:
    %y = add.i32 %x, 7 [wrap]
    jmp .merge(%y)
.dead:
    %z = add.i32 %y, 1 [wrap]
    jmp .merge(%z)
.merge(%out: i32):
    ret %out
}

// 6. Loop with negative i32 immediate addition (tests loop_vec i32 imm sign-ext)
//    and i64 shifted-index store (tests loop_vec i64 shift-wrap guard).
func @test_loop_vec_neg_imm(%arr: ptr, %n: i64) -> i32 [nounwind] {
.entry:
    jmp .loop(0)
.loop(%i: i64):
    %ok = cmp_lt_s.i64 %i, %n
    br %ok, .body, .exit
.body:
    %off = shl.i64 %i, 2 [mask]
    %p = ptr_add.i64 %arr, %off
    %v = load.i32 %p, align 4
    %nv = add.i32 %v, -3 [wrap]
    store.i32 %nv, %p, align 4
    %ni = add.i64 %i, 1 [wrap]
    jmp .loop(%ni)
.exit:
    %p0 = ptr_add.i64 %arr, 0
    %r0 = load.i32 %p0, align 4
    %p7 = ptr_add.i64 %arr, 28
    %r7 = load.i32 %p7, align 4
    %sum = add.i32 %r0, %r7 [wrap]
    ret %sum
}

// 7. Large >4KB stack frame + many live GP values (tests ARM64 x16/x17 spill scratch isolation).
func @test_large_frame_spill(%a: i64, %b: i64) -> i64 [nounwind] {
.entry:
    %big = alloca [5000 x i8], align 8
    %v0 = add.i64 %a, 1 [wrap]
    %v1 = add.i64 %b, 2 [wrap]
    %v2 = add.i64 %v0, %v1 [wrap]
    %v3 = xor.i64 %v2, 0x55
    %v4 = add.i64 %v3, %v0 [wrap]
    %v5 = add.i64 %v4, %v1 [wrap]
    %v6 = add.i64 %v5, %v2 [wrap]
    %v7 = add.i64 %v6, %v3 [wrap]
    %v8 = add.i64 %v7, %v4 [wrap]
    %v9 = add.i64 %v8, %v5 [wrap]
    %v10 = add.i64 %v9, %v6 [wrap]
    %v11 = add.i64 %v10, %v7 [wrap]
    %v12 = add.i64 %v11, %v8 [wrap]
    %v13 = add.i64 %v12, %v9 [wrap]
    %v14 = add.i64 %v13, %v10 [wrap]
    %v15 = add.i64 %v14, %v11 [wrap]
    %v16 = add.i64 %v15, %v12 [wrap]
    %v17 = add.i64 %v16, %v13 [wrap]
    %v18 = add.i64 %v17, %v14 [wrap]
    %v19 = add.i64 %v18, %v15 [wrap]
    %v20 = add.i64 %v19, %v16 [wrap]
    %v21 = add.i64 %v20, %v17 [wrap]
    %v22 = add.i64 %v21, %v18 [wrap]
    %v23 = add.i64 %v22, %v19 [wrap]
    %p = ptr_add.i64 %big, 4900
    store.i64 %v23, %p, align 4
    %ld = load.i64 %p, align 4
    %s1 = add.i64 %ld, %v0 [wrap]
    %s2 = add.i64 %s1, %v5 [wrap]
    %s3 = add.i64 %s2, %v11 [wrap]
    %s4 = add.i64 %s3, %v17 [wrap]
    %s5 = add.i64 %s4, %v22 [wrap]
    %out = and.i64 %s5, 0xffff
    ret %out
}

export func @main() -> i32 cc "c" {
.entry:
    %r1 = call @test_negzero_globals()
    %ok1 = cmp_eq.i32 %r1, 1

    %r2a = call @test_gvn_inttoptr_and_inbounds(1)
    %r2b = call @test_gvn_inttoptr_and_inbounds(0)
    %ok2a = cmp_eq.i32 %r2a, 53
    %ok2b = cmp_eq.i32 %r2b, 33

    %r3a = call @test_diamond_negzero(1, -0.0)
    %r3b = call @test_diamond_negzero(0, -0.0)
    %ok3a = cmp_eq.i64 %r3a, 0x8000000000000000
    %ok3b = cmp_eq.i64 %r3b, 0

    %pair = alloca @Pair16, align 8
    %pp0 = field_ptr @Pair16, %pair, 0
    %pp1 = field_ptr @Pair16, %pair, 1
    store.i64 10, %pp0, align 8
    store.i64 20, %pp1, align 8
    %r4 = call @test_byval_rec(%pair, 5)
    // 10+5 + 20+10 = 45
    %ok4 = cmp_eq.i64 %r4, 45

    %r5 = call @test_unreach_ssa(35)
    %ok5 = cmp_eq.i32 %r5, 42

    %buf = alloca [16 x i32], align 16
    jmp .init(0)
.init(%k: i64):
    %klt = cmp_lt_s.i64 %k, 16
    br %klt, .init_b, .init_done
.init_b:
    %koff = shl.i64 %k, 2 [mask]
    %kp = ptr_add.i64 %buf, %koff
    %kv = trunc.i32 %k
    %kv2 = add.i32 %kv, 10 [wrap]
    store.i32 %kv2, %kp, align 4
    %kn = add.i64 %k, 1 [wrap]
    jmp .init(%kn)
.init_done:
    %r6 = call @test_loop_vec_neg_imm(%buf, 16)
    // arr[0]=10-3=7, arr[7]=17-3=14 -> 21
    %ok6 = cmp_eq.i32 %r6, 21

    %r7 = call @test_large_frame_spill(3, 5)
    %ok7 = cmp_ne.i64 %r7, 0

    %m1 = and.i8 %ok1, %ok2a
    %m2 = and.i8 %ok2b, %ok3a
    %m3 = and.i8 %ok3b, %ok4
    %m4 = and.i8 %ok5, %ok6
    %m5 = and.i8 %m1, %m2
    %m6 = and.i8 %m3, %m4
    %m7 = and.i8 %m5, %m6
    %all = and.i8 %m7, %ok7
    %bad = xor.i8 %all, 1
    %rc = zext.i32 %bad
    ret %rc
}
