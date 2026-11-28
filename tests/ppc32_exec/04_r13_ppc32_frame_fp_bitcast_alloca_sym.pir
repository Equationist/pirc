// Round 13 PPC32 Big-Endian backend regression test:
// - Large-frame (>32 KiB) spilled parallel-copy cycle (r11/r30 scratch isolation)
// - bitcast.f32/f64 <imm_int>, bitcast.i64 %x / <imm_int>, bitcast.v2i64
// - Default (flagless) saturating fptosi/fptoui & [trap] negative fractions
// - alloca_dyn with align > 16 (32B / 64B)
// - Register-like & non-bare quoted symbols (@r0, @f1, @"foo-bar", @"r13::fn")
// - SysV SVR4 i64 gp_i==7 spill + trailing GP arg + variadic va_start gpr=8
// - 64-bit scalarized vector (v2f32) + i64 variadic alignment
// - 3B / 5B / 6B / 7B right-justified retbuf returns & acquire fence (lwsync)
target "powerpc-unknown-elf"

@g_n = global i32 = 3, align 4
@r0 = global i32 = 7, align 4
@"foo-bar" = global i32 = 35, align 4

type @BigBuf = [40000 x i8]
type @S3 = [3 x i8]
type @S5 = [5 x i8]
type @S6 = [6 x i8]
type @S7 = [7 x i8]

func @"r13::fn"(%x: i32) -> i32 nounwind {
.entry:
    %p = addr @"foo-bar"
    %v = load.i32 %p, align 4
    %s = add.i32 %x, %v
    ret %s
}

func @f1(%x: i32) -> i32 nounwind {
.entry:
    %r = call @"r13::fn"(%x)
    ret %r
}

func @big_pcopy(%n: i32) -> i32 nounwind {
.entry:
    %big = alloca @BigBuf, align 16
    atomic_store.i8 42, %big [seq_cst]
    fence [acquire]
    jmp .loop(%n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28)
.loop(%i: i32, %v0: i32, %v1: i32, %v2: i32, %v3: i32, %v4: i32, %v5: i32, %v6: i32, %v7: i32, %v8: i32, %v9: i32, %v10: i32, %v11: i32, %v12: i32, %v13: i32, %v14: i32, %v15: i32, %v16: i32, %v17: i32, %v18: i32, %v19: i32, %v20: i32, %v21: i32, %v22: i32, %v23: i32, %v24: i32, %v25: i32, %v26: i32, %v27: i32):
    %done = cmp_eq.i32 %i, 0
    br %done, .exit, .step
.step:
    %ni = sub.i32 %i, 1
    jmp .loop(%ni, %v1, %v2, %v3, %v4, %v5, %v6, %v7, %v8, %v9, %v10, %v11, %v12, %v13, %v14, %v15, %v16, %v17, %v18, %v19, %v20, %v21, %v22, %v23, %v24, %v25, %v26, %v27, %v0)
.exit:
    %b0 = atomic_load.i8 %big [acquire]
    %b0i = zext.i32 %b0
    %w = mul.i32 %v0, 100
    %out = add.i32 %w, %b0i
    ret %out
}

func @f2i_def(%x: f64) -> i32 nounwind {
.entry:
    %r = fptosi.i32 %x
    ret %r
}

func @f2u_def(%x: f64) -> i32 nounwind {
.entry:
    %r = fptoui.i32 %x
    ret %r
}

func @f2i8_def(%x: f64) -> i8 nounwind {
.entry:
    %r = fptosi.i8 %x
    ret %r
}

func @f2u_trap(%x: f64) -> i32 {
.entry:
    %r = fptoui.i32 %x [trap]
    ret %r
}

func @f2i8_trap(%x: f64) -> i8 {
.entry:
    %r = fptosi.i8 %x [trap]
    ret %r
}

func @f2i32_trap(%x: f64) -> i32 {
.entry:
    %r = fptosi.i32 %x [trap]
    ret %r
}

func @check_alloca_dyn(%n: i32) -> i32 nounwind {
.entry:
    %p32 = alloca_dyn %n, align 32
    %p64 = alloca_dyn %n, align 64
    %a32 = ptrtoint.i32 %p32
    %a64 = ptrtoint.i32 %p64
    %m32 = and.i32 %a32, 31
    %m64 = and.i32 %a64, 63
    %r = or.i32 %m32, %m64
    ret %r
}

func @gp7_i64_va(%a1: i32, %a2: i32, %a3: i32, %a4: i32, %a5: i32, %a6: i32, %a7: i32, %q: i64, ...) -> i64 cc "c" nounwind {
.entry:
    %ap = alloca va_list
    va_start %ap
    %t1 = va_arg.i32 %ap
    %t2 = va_arg.i32 %ap
    va_end %ap
    %s1 = add.i32 %a1, %a2
    %s2 = add.i32 %a3, %a4
    %s3 = add.i32 %a5, %a6
    %s4 = add.i32 %a7, %t1
    %s5 = add.i32 %s1, %s2
    %s6 = add.i32 %s3, %s4
    %s7 = add.i32 %s5, %s6
    %s8 = add.i32 %s7, %t2
    %se = sext.i64 %s8
    %tot = add.i64 %se, %q
    ret %tot
}

func @va_v2f32_i64(%n: i32, ...) -> i64 cc "c" nounwind {
.entry:
    %ap = alloca va_list
    va_start %ap
    %a2 = va_arg.i32 %ap
    %a3 = va_arg.i32 %ap
    %a4 = va_arg.i32 %ap
    %a5 = va_arg.i32 %ap
    %vf = va_arg.v2f32 %ap
    %a6 = va_arg.i32 %ap
    %q  = va_arg.i64 %ap
    %z  = va_arg.i32 %ap
    va_end %ap
    %f0 = extract.v2f32 %vf, 0
    %i0 = fptosi.i32 %f0 [sat]
    %s1 = add.i32 %n, %a2
    %s2 = add.i32 %a3, %a4
    %s3 = add.i32 %a5, %a6
    %s4 = add.i32 %i0, %z
    %s5 = add.i32 %s1, %s2
    %s6 = add.i32 %s3, %s4
    %s7 = add.i32 %s5, %s6
    %se = sext.i64 %s7
    %tot = add.i64 %se, %q
    ret %tot
}

func @make_s3(%out: retbuf(@S3)) -> void cc "c" nounwind {
.entry:
    store.i8 0x11, %out, align 1
    %p1 = ptr_add.i32 %out, 1
    store.i8 0x22, %p1, align 1
    %p2 = ptr_add.i32 %out, 2
    store.i8 0x33, %p2, align 1
    ret
}

func @make_s5(%out: retbuf(@S5)) -> void cc "c" nounwind {
.entry:
    store.i8 0x11, %out, align 1
    %p1 = ptr_add.i32 %out, 1
    store.i8 0x22, %p1, align 1
    %p2 = ptr_add.i32 %out, 2
    store.i8 0x33, %p2, align 1
    %p3 = ptr_add.i32 %out, 3
    store.i8 0x44, %p3, align 1
    %p4 = ptr_add.i32 %out, 4
    store.i8 0x55, %p4, align 1
    ret
}

func @id_i64(%x: i64) -> i64 nounwind {
.entry:
    %y = bitcast.i64 %x
    %z = bitcast.i64 5
    %r = add.i64 %y, %z
    ret %r
}

func @id_v2i64(%v: v2i64) -> i64 nounwind {
.entry:
    %w = bitcast.v2i64 %v
    %e0 = extract.v2i64 %w, 0
    %e1 = extract.v2i64 %w, 1
    %s = add.i64 %e0, %e1
    ret %s
}

export func @main() -> i32 cc "c" {
.entry:
    // 1. Large-frame spilled pcopy cycle
    %pn = addr @g_n
    %n = atomic_load.i32 %pn [seq_cst]
    %r1 = call @big_pcopy(%n)
    %ok1 = cmp_eq.i32 %r1, 442
    br %ok1, .t2, .f1
.t2:
    // 2. bitcast imm -> f32/f64 & same-type bitcast.i64 / v2i64
    %bf32 = bitcast.f32 0x3F800000
    %bi32 = fptosi.i32 %bf32 [sat]
    %bf64 = bitcast.f64 0x4000000000000000
    %bi64 = fptosi.i32 %bf64 [sat]
    %q1 = call @id_i64(37)
    %v0 = splat.v2i64 11
    %q2 = call @id_v2i64(%v0)
    %t2a = add.i32 %bi32, %bi64
    %q1t = trunc.i32 %q1
    %q2t = trunc.i32 %q2
    %t2b = add.i32 %q1t, %q2t
    %t2c = add.i32 %t2a, %t2b
    %ok2 = cmp_eq.i32 %t2c, 67
    br %ok2, .t3, .f2
.t3:
    // 3. Default saturating fptosi/fptoui + [trap] negative fractions
    %nan = bitcast.f64 0x7FF8000000000000
    %d1 = call @f2i_def(%nan)
    %d2 = call @f2u_def(-5.0)
    %d3 = call @f2i8_def(300.0)
    %tr1 = call @f2u_trap(-0.5)
    %tr2 = call @f2i8_trap(-128.5)
    %tr3 = call @f2i32_trap(-2147483648.5)
    %c3a = cmp_eq.i32 %d1, 0
    %c3b = cmp_eq.i32 %d2, 0
    %c3c = cmp_eq.i8 %d3, 127
    %c3d = cmp_eq.i32 %tr1, 0
    %c3e = cmp_eq.i8 %tr2, -128
    %c3f = cmp_eq.i32 %tr3, -2147483648
    %m3a = and.i8 %c3a, %c3b
    %m3b = and.i8 %c3c, %c3d
    %m3c = and.i8 %c3e, %c3f
    %m3d = and.i8 %m3a, %m3b
    %ok3 = and.i8 %m3d, %m3c
    br %ok3, .t4, .f3
.t4:
    // 4. alloca_dyn align > 16
    %alr = call @check_alloca_dyn(16)
    %ok4 = cmp_eq.i32 %alr, 0
    br %ok4, .t5, .f4
.t5:
    // 5. Register-like & quoted non-bare symbols
    %pr0 = addr @r0
    %vr0 = load.i32 %pr0, align 4
    %sr = call @f1(%vr0)
    %ok5 = cmp_eq.i32 %sr, 42
    br %ok5, .t6, .f5
.t6:
    // 6. gp_i == 7 i64 spill + variadic args
    %va1 = call @gp7_i64_va(1, 2, 3, 4, 5, 6, 7, 1000, 100: i32, 200: i32)
    %ok6 = cmp_eq.i64 %va1, 1328
    br %ok6, .t7, .f6
.t7:
    // 7. v2f32 + i64 variadic placement
    %vf = splat.v2f32 5.0
    %va2 = call @va_v2f32_i64(1, 2: i32, 3: i32, 4: i32, 5: i32, %vf: v2f32, 6: i32, 0x100000020: i64, 99: i32)
    %ok7 = cmp_eq.i64 %va2, 0x10000009d
    br %ok7, .t8, .f7
.t8:
    // 8. 3B & 5B retbuf returns
    %b3 = alloca @S3
    call @make_s3(%b3)
    %b3_0 = load.i8 %b3, align 1
    %b3_2p = ptr_add.i32 %b3, 2
    %b3_2 = load.i8 %b3_2p, align 1
    %b5 = alloca @S5
    call @make_s5(%b5)
    %b5_0 = load.i8 %b5, align 1
    %b5_4p = ptr_add.i32 %b5, 4
    %b5_4 = load.i8 %b5_4p, align 1
    %s8a = cmp_eq.i8 %b3_0, 0x11
    %s8b = cmp_eq.i8 %b3_2, 0x33
    %s8c = cmp_eq.i8 %b5_0, 0x11
    %s8d = cmp_eq.i8 %b5_4, 0x55
    %m8a = and.i8 %s8a, %s8b
    %m8b = and.i8 %s8c, %s8d
    %ok8 = and.i8 %m8a, %m8b
    br %ok8, .pass, .f8
.f1: ret 1
.f2: ret 2
.f3: ret 3
.f4: ret 4
.f5: ret 5
.f6: ret 6
.f7: ret 7
.f8: ret 8
.pass: ret 0
}
