target "avr-unknown-elf"

// Round 14 AVR backend, lowering, alias, soft-float, and runtime regression test:
//   1. Large >63B frame + spilled phi swap/3-way cycles (MI_AVR_ST Z must not clobber X)
//   2. High-register-pressure mul_wide_u/s.i16 + mul.i16 [trap_s] (4-op spill pinning)
//   3. Register-named symbols (@sp, @r1) in direct calls & global pm() tables
//   4. i128 cc "c" sret return, bitcast.i32 (val & imm)
//   5. soft_float default saturation + [trap] fractional bounds (-0.5, -128.5, -2147483648.5)
//   6. alias.c -O2 regressions (negative i16 offset 65532, mul.i16 stride, shl.i16 16 [mask], @g + 65536)
//   7. 5B/6B retbuf, fixed i8 variadic param, spilled i8 & odd A3 struct, printf >= 2^32

type @S5 = { i8, i8, i8, i8, i8 }
type @S6 = { i16, i16, i16 }
type @A3 = { i8, i8, i8 }
sig @fn_sig = (i16) -> i16 cc "c" [nounwind]

@g_i16 = global i16 = 42, align 2
@fn_tbl = data align 1 { ptr @r1 }
@fmt_big = data align 1 { "%.2f\n", 0 }

extern func @printf(%fmt: ptr, ...) -> i32 cc "c" [nounwind]

func @sp(%x: i16) -> i16 cc "c" [nounwind] {
.entry:
    %r = add.i16 %x, 1
    ret %r
}

func @r1(%x: i16) -> i16 cc "c" [nounwind] {
.entry:
    %r = add.i16 %x, 33
    ret %r
}

// Test 1: Large frame (>80B) with spilled phi swap/3-way cycles
weak func @test_pcopy_large(%n: i16) -> i16 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 80, align 1
    store.i16 %n, %buf, align 1
    %p1 = ptr_add.i16 %buf, 78
    store.i16 7, %p1, align 1
    jmp .loop(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13)
.loop(%i: i16, %a: i16, %b: i16, %c: i16, %d: i16, %e: i16, %f: i16, %g: i16, %h: i16, %k: i16, %m: i16, %q: i16, %r: i16, %s: i16):
    %done = cmp_ge_s.i16 %i, %n
    br %done, .exit, .body
.body:
    %ni = add.i16 %i, 1
    jmp .loop(%ni, %b, %a, %d, %e, %c, %g, %f, %k, %h, %q, %m, %s, %r)
.exit:
    %t0 = add.i16 %a, %b
    %t1 = add.i16 %t0, %c
    %t2 = add.i16 %t1, %d
    %t3 = add.i16 %t2, %e
    %t4 = add.i16 %t3, %f
    %t5 = add.i16 %t4, %g
    %t6 = add.i16 %t5, %h
    %t7 = add.i16 %t6, %k
    %t8 = add.i16 %t7, %m
    %t9 = add.i16 %t8, %q
    %t10 = add.i16 %t9, %r
    %t11 = add.i16 %t10, %s
    %lv = load.i16 %p1, align 1
    %out = add.i16 %t11, %lv
    ret %out
}

// Test 2: High-pressure mul_wide_u.i16 + mul_wide_s.i16 + mul.i16 [trap_s] on large frame
weak func @test_mul_wide(%x: i16, %y: i16) -> i16 cc "c" [nounwind] {
.entry:
    %pad = alloca.ptr 80, align 1
    store.i16 %x, %pad, align 1
    %v1 = add.i16 %x, 1
    %v2 = add.i16 %x, 2
    %v3 = add.i16 %x, 3
    %v4 = add.i16 %x, 4
    %v5 = add.i16 %x, 5
    %v6 = add.i16 %x, 6
    %v7 = add.i16 %x, 7
    %v8 = add.i16 %x, 8
    %v9 = add.i16 %x, 9
    %v10 = add.i16 %x, 10
    %v11 = add.i16 %x, 11
    %v12 = add.i16 %x, 12
    %lo, %hi = mul_wide_u.i16 %x, %y
    %slo, %shi = mul_wide_s.i16 %v1, %y
    %tm = mul.i16 %v2, 3 [trap_s]
    %s0 = add.i16 %lo, %hi
    %s1 = add.i16 %s0, %slo
    %s2 = add.i16 %s1, %shi
    %s3 = add.i16 %s2, %tm
    %s4 = add.i16 %s3, %v3
    %s5 = add.i16 %s4, %v4
    %s6 = add.i16 %s5, %v5
    %s7 = add.i16 %s6, %v6
    %s8 = add.i16 %s7, %v7
    %s9 = add.i16 %s8, %v8
    %s10 = add.i16 %s9, %v9
    %s11 = add.i16 %s10, %v10
    %s12 = add.i16 %s11, %v11
    %s13 = add.i16 %s12, %v12
    ret %s13
}

// Test 4: i128 cc "c" return + bitcast.i32
weak func @make_i128(%a: i16) -> i128 cc "c" [nounwind] {
.entry:
    %e = zext.i128 %a
    %r = add.i128 %e, 5
    ret %r
}

weak func @test_bitcast_i32(%x: i32) -> i32 cc "c" [nounwind] {
.entry:
    %b1 = bitcast.i32 %x
    %b2 = bitcast.i32 7
    %r = add.i32 %b1, %b2
    ret %r
}

// Test 6: alias.c -O2 regressions (negative i16 offset, mul.i16 stride, shl.i16 16 [mask])
weak func @alias_neg_off(%a: ptr) -> i16 cc "c" [nounwind] {
.entry:
    %p1 = ptr_add.i16 %a, 4 [inbounds]
    %p2 = ptr_add.i16 %p1, 65532 [inbounds]
    store.i16 10, %a, align 2
    store.i16 42, %p2, align 2
    %v = load.i16 %a, align 2
    ret %v
}

weak func @alias_mul_stride(%a: ptr, %idx: i16) -> i16 cc "c" [nounwind] {
.entry:
    %off = mul.i16 %idx, 65532 [unsafe_s]
    %p = ptr_add.i16 %a, %off [inbounds]
    %q = ptr_add.i16 %a, 4 [inbounds]
    store.i16 10, %q, align 2
    store.i16 42, %p, align 2
    %v = load.i16 %q, align 2
    ret %v
}

weak func @alias_shl16(%a: ptr, %idx: i16) -> i16 cc "c" [nounwind] {
.entry:
    %off = shl.i16 %idx, 16 [mask]
    %p = ptr_add.i16 %a, %off
    %q = ptr_add.i16 %a, 2
    store.i16 10, %q, align 2
    store.i16 42, %p, align 2
    %v = load.i16 %q, align 2
    ret %v
}

// Test 7: 5B/6B retbuf, fixed i8 variadic, spilled i8 & odd A3
weak func @ret_s5(%out: retbuf(@S5), %base: i8) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S5, %out, 0
    %p1 = field_ptr @S5, %out, 1
    %p2 = field_ptr @S5, %out, 2
    %p3 = field_ptr @S5, %out, 3
    %p4 = field_ptr @S5, %out, 4
    store.i8 %base, %p0, align 1
    %b1 = add.i8 %base, 1
    %b2 = add.i8 %base, 2
    %b3 = add.i8 %base, 3
    %b4 = add.i8 %base, 4
    store.i8 %b1, %p1, align 1
    store.i8 %b2, %p2, align 1
    store.i8 %b3, %p3, align 1
    store.i8 %b4, %p4, align 1
    ret
}

weak func @va_fixed_i8(%tag: i8, %b: i16, ...) -> i16 cc "c" [nounwind] {
.entry:
    %ap = alloca va_list
    va_start %ap
    %v1 = va_arg.i16 %ap
    %v2 = va_arg.i16 %ap
    va_end %ap
    %t16 = zext.i16 %tag
    %s0 = add.i16 %t16, %b
    %s1 = add.i16 %s0, %v1
    %s2 = add.i16 %s1, %v2
    ret %s2
}

weak func @spilled_i8_a3(%a0: i16, %a1: i16, %a2: i16, %a3: i16, %a4: i16, %a5: i16, %a6: i16, %a7: i16, %a8: i16, %b9: i8, %s10: byval(@A3), %a11: i16) -> i16 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @A3, %s10, 0
    %p1 = field_ptr @A3, %s10, 1
    %p2 = field_ptr @A3, %s10, 2
    %c0 = load.i8 %p0, align 1
    %c1 = load.i8 %p1, align 1
    %c2 = load.i8 %p2, align 1
    %z9 = zext.i16 %b9
    %z0 = zext.i16 %c0
    %z1 = zext.i16 %c1
    %z2 = zext.i16 %c2
    %r0 = add.i16 %a0, %a8
    %r1 = add.i16 %r0, %z9
    %r2 = add.i16 %r1, %z0
    %r3 = add.i16 %r2, %z1
    %r4 = add.i16 %r3, %z2
    %r5 = add.i16 %r4, %a11
    ret %r5
}

export func @main() -> i16 {
.entry:
    // 1. Large frame pcopy cycles: sum(1..13)=91 + 7 = 98
    %r1 = call @test_pcopy_large(6)
    %ok1 = cmp_eq.i16 %r1, 98
    br %ok1, .c2, .fail1

.c2:
    // 2. High-pressure mul_wide
    %r2 = call @test_mul_wide(100, 300)
    // lo = (30000 & 0xFFFF)=30000, hi = 0// slo = (30300 & 0xFFFF)=30300, shi = 0//
    // tm = 102*3 = 306// sum(103..112) = 1075// total mod 65536 = 61681 (-3855)
    %ok2 = cmp_eq.i16 %r2, 61681
    br %ok2, .c3, .fail2

.c3:
    // 3. Register-named symbols + global pm() table
    %rs = call @sp(41)
    %tbl = addr @fn_tbl
    %fp = load.ptr %tbl, align 1
    %rr = call_indirect @fn_sig, %fp(%rs)
    %ok3 = cmp_eq.i16 %rr, 75
    br %ok3, .c4, .fail3

.c4:
    // 4. i128 cc "c" return + bitcast.i32
    %w128 = call @make_i128(37)
    %w16 = trunc.i16 %w128
    %bc = call @test_bitcast_i32(70)
    %bc16 = trunc.i16 %bc
    %c4sum = add.i16 %w16, %bc16
    %ok4 = cmp_eq.i16 %c4sum, 119
    br %ok4, .c5, .fail4

.c5:
    // 5. soft_float default saturation + [trap] fractional bounds
    %f300 = const.f64 300.0
    %sat_s8 = fptosi.i8 %f300
    %sat_u8 = fptoui.i8 %f300
    %fneg05 = const.f64 -0.5
    %tu = fptoui.i32 %fneg05 [trap]
    %fneg1285 = const.f64 -128.5
    %ts8 = fptosi.i8 %fneg1285 [trap]
    %fmin32 = const.f64 -2147483648.5
    %ts32 = fptosi.i32 %fmin32 [trap]
    %ok5a = cmp_eq.i8 %sat_s8, 127
    %ok5b = cmp_eq.i8 %sat_u8, 255
    %ok5c = cmp_eq.i32 %tu, 0
    %ok5d = cmp_eq.i8 %ts8, -128
    %ok5e = cmp_eq.i32 %ts32, -2147483648
    %a5_0 = and.i8 %ok5a, %ok5b
    %a5_1 = and.i8 %ok5c, %ok5d
    %a5_2 = and.i8 %a5_0, %a5_1
    %ok5 = and.i8 %a5_2, %ok5e
    br %ok5, .c6, .fail5

.c6:
    // 6. alias.c -O2 + @g_i16 + 65536 wrap
    %abuf = alloca.ptr 8, align 2
    %al1 = call @alias_neg_off(%abuf)
    %al2 = call @alias_mul_stride(%abuf, -1)
    %al3 = call @alias_shl16(%abuf, 2)
    %gp0 = addr @g_i16 + 65536
    %gp1 = ptr_add.i16 %gp0, 0
    %al4 = load.i16 %gp1, align 2
    %al_s0 = add.i16 %al1, %al2
    %al_s1 = add.i16 %al3, %al4
    %al_tot = add.i16 %al_s0, %al_s1
    %ok6 = cmp_eq.i16 %al_tot, 168
    br %ok6, .c7, .fail6

.c7:
    // 7. 5B retbuf + fixed i8 variadic + spilled i8/A3 + printf >= 2^32
    %s5 = alloca @S5
    call @ret_s5(%s5, 10)
    %s5p0 = field_ptr @S5, %s5, 0
    %s5p4 = field_ptr @S5, %s5, 4
    %s5v0 = load.i8 %s5p0, align 1
    %s5v4 = load.i8 %s5p4, align 1
    %va_r = call @va_fixed_i8(5, 100, 200: i16, 300: i16)
    %a3 = alloca @A3
    %a3p0 = field_ptr @A3, %a3, 0
    %a3p1 = field_ptr @A3, %a3, 1
    %a3p2 = field_ptr @A3, %a3, 2
    store.i8 11, %a3p0, align 1
    store.i8 22, %a3p1, align 1
    store.i8 33, %a3p2, align 1
    %sp_r = call @spilled_i8_a3(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, %a3, 100)
    %fm = addr @fmt_big
    %big = const.f64 10000000000.0
    %pr = call @printf(%fm, %big: f64)
    %ok7a = cmp_eq.i8 %s5v0, 10
    %ok7b = cmp_eq.i8 %s5v4, 14
    %ok7c = cmp_eq.i16 %va_r, 605
    %ok7d = cmp_eq.i16 %sp_r, 186
    %t7_0 = and.i8 %ok7a, %ok7b
    %t7_1 = and.i8 %ok7c, %ok7d
    %ok7 = and.i8 %t7_0, %t7_1
    br %ok7, .pass, .fail7

.pass:
    ret 0
.fail1:
    ret 1
.fail2:
    ret 2
.fail3:
    ret 3
.fail4:
    ret 4
.fail5:
    ret 5
.fail6:
    ret 6
.fail7:
    ret 7
}
