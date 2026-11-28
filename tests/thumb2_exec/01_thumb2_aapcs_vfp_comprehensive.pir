target "thumbv7em-none-eabihf"

// Comprehensive ARM Thumb-2 (ARMv7E-M, Cortex-M7 FPv5-D16) backend test:
// 1. Hardware sdiv/udiv, umull/smull (mul_wide.i32), clz, ctz, popcnt, bswap
// 2. AAPCS32-VFP backfilling (f32, f64, f32 -> s0, d1, s1) & >16 s-reg stack spill
// 3. HFA structs {f32,f32,f32,f32} in s0..s3 and {f64,f64,f64} in d0..d2
// 4. Non-HFA structs: <=4B in r0, >4B sret, <=16B split across r0..r3+stack, >16B ptr
// 5. i64 even-register alignment (r0:r1, r2:r3, skip r1/r3)
// 6. Thumb interworking function pointers (const.ptr @fn + table + call_indirect blx)
// 7. Dense Thumb-2 jump table switch (ldr.w pc, [r12, idx, lsl #2])
// 8. AAPCS32 variadics (r0..r3 save area + stack, including variadic f64 & i64)
// 9. Dynamic alloca_dyn + stacksave + stackrestore (8-byte SP alignment)

type @Hfa4f = { f32, f32, f32, f32 }
type @Hfa3d = { f64, f64, f64 }
type @Small3 = { i8, i16 }
type @Mid12  = { i32, i32, i32 }
type @Big24  = { i32, i32, i32, i32, i32, i32 }

@fn_tbl = data align 4 { ptr @fn_add, ptr @fn_sub }

func @fn_add(%a: i32, %b: i32) -> i32 cc "c" [nounwind] {
.entry:
    %r = add.i32 %a, %b
    ret %r
}

func @fn_sub(%a: i32, %b: i32) -> i32 cc "c" [nounwind] {
.entry:
    %r = sub.i32 %a, %b
    ret %r
}

sig @bin32 = (i32, i32) -> i32 cc "c" [nounwind]

// AAPCS32-VFP backfilling: %a -> s0, %b -> d1(s2:s3), %c -> s1!
func @vfp_backfill(%a: f32, %b: f64, %c: f32, %d: f64, %e: f32) -> f64 cc "c" [nounwind] {
.entry:
    %ad = fpext.f64 %a
    %cd = fpext.f64 %c
    %ed = fpext.f64 %e
    %s1 = fadd.f64 %ad, %b
    %s2 = fadd.f64 %s1, %cd
    %s3 = fadd.f64 %s2, %d
    %s4 = fadd.f64 %s3, %ed
    ret %s4
}

func @make_hfa4f(%out: retbuf(@Hfa4f), %a: f32, %b: f32, %c: f32, %d: f32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4f, %out, 0
    %p1 = field_ptr @Hfa4f, %out, 1
    %p2 = field_ptr @Hfa4f, %out, 2
    %p3 = field_ptr @Hfa4f, %out, 3
    store.f32 %a, %p0, align 4
    store.f32 %b, %p1, align 4
    store.f32 %c, %p2, align 4
    store.f32 %d, %p3, align 4
    ret
}

func @sum_hfa4f(%tag: f32, %x: f64, %h: byval(@Hfa4f)) -> f32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4f, %h, 0
    %p1 = field_ptr @Hfa4f, %h, 1
    %p2 = field_ptr @Hfa4f, %h, 2
    %p3 = field_ptr @Hfa4f, %h, 3
    %a = load.f32 %p0, align 4
    %b = load.f32 %p1, align 4
    %c = load.f32 %p2, align 4
    %d = load.f32 %p3, align 4
    %xf = fptrunc.f32 %x
    %s0 = fadd.f32 %tag, %xf
    %s1 = fadd.f32 %a, %b
    %s2 = fadd.f32 %c, %d
    %s3 = fadd.f32 %s1, %s2
    %r  = fadd.f32 %s0, %s3
    ret %r
}

func @make_hfa3d(%out: retbuf(@Hfa3d), %a: f64, %b: f64, %c: f64) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa3d, %out, 0
    %p1 = field_ptr @Hfa3d, %out, 1
    %p2 = field_ptr @Hfa3d, %out, 2
    store.f64 %a, %p0, align 8
    store.f64 %b, %p1, align 8
    store.f64 %c, %p2, align 8
    ret
}

func @sum_hfa3d(%h: byval(@Hfa3d)) -> f64 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa3d, %h, 0
    %p1 = field_ptr @Hfa3d, %h, 1
    %p2 = field_ptr @Hfa3d, %h, 2
    %a = load.f64 %p0, align 8
    %b = load.f64 %p1, align 8
    %c = load.f64 %p2, align 8
    %s1 = fadd.f64 %a, %b
    %r  = fadd.f64 %s1, %c
    ret %r
}

func @make_small3(%out: retbuf(@Small3), %a: i8, %b: i16) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Small3, %out, 0
    %p1 = field_ptr @Small3, %out, 1
    store.i8 %a, %p0, align 1
    store.i16 %b, %p1, align 2
    ret
}

// Mid12 (3 words) after 2 GP args -> splits r2, r3 + stack!
func @eval_mid12(%k0: i32, %k1: i32, %m: byval(@Mid12)) -> i32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Mid12, %m, 0
    %p1 = field_ptr @Mid12, %m, 1
    %p2 = field_ptr @Mid12, %m, 2
    %a = load.i32 %p0, align 4
    %b = load.i32 %p1, align 4
    %c = load.i32 %p2, align 4
    %s0 = add.i32 %k0, %k1
    %s1 = add.i32 %a, %b
    %s2 = add.i32 %s0, %s1
    %r  = add.i32 %s2, %c
    ret %r
}

func @make_big24(%out: retbuf(@Big24), %v: i32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Big24, %out, 0
    %p1 = field_ptr @Big24, %out, 1
    %p2 = field_ptr @Big24, %out, 2
    %p3 = field_ptr @Big24, %out, 3
    %p4 = field_ptr @Big24, %out, 4
    %p5 = field_ptr @Big24, %out, 5
    store.i32 %v, %p0, align 4
    %v1 = add.i32 %v, 1
    store.i32 %v1, %p1, align 4
    %v2 = add.i32 %v, 2
    store.i32 %v2, %p2, align 4
    %v3 = add.i32 %v, 3
    store.i32 %v3, %p3, align 4
    %v4 = add.i32 %v, 4
    store.i32 %v4, %p4, align 4
    %v5 = add.i32 %v, 5
    store.i32 %v5, %p5, align 4
    ret
}

func @sum_big24(%b: byval(@Big24)) -> i32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Big24, %b, 0
    %p1 = field_ptr @Big24, %b, 1
    %p2 = field_ptr @Big24, %b, 2
    %p3 = field_ptr @Big24, %b, 3
    %p4 = field_ptr @Big24, %b, 4
    %p5 = field_ptr @Big24, %b, 5
    %a0 = load.i32 %p0, align 4
    %a1 = load.i32 %p1, align 4
    %a2 = load.i32 %p2, align 4
    %a3 = load.i32 %p3, align 4
    %a4 = load.i32 %p4, align 4
    %a5 = load.i32 %p5, align 4
    %s0 = add.i32 %a0, %a1
    %s1 = add.i32 %a2, %a3
    %s2 = add.i32 %a4, %a5
    %s3 = add.i32 %s0, %s1
    %r  = add.i32 %s3, %s2
    ret %r
}

// i64 even-reg alignment: %t in r0, %x in r2:r3, %y on 8B stack!
func @i64_align_test(%t: i32, %x: i64, %y: i64) -> i64 cc "c" [nounwind] {
.entry:
    %t64 = zext.i64 %t
    %s1 = add.i64 %x, %y
    %r  = add.i64 %s1, %t64
    ret %r
}

func @dense_sw(%x: i32) -> i32 cc "c" [nounwind] {
.entry:
    switch.i32 %x, default .def [
        10 -> .c10,
        11 -> .c11,
        12 -> .c12,
        13 -> .c13,
        14 -> .c14
    ]
.c10:
    ret 100
.c11:
    ret 200
.c12:
    ret 300
.c13:
    ret 400
.c14:
    ret 500
.def:
    ret -1
}

func @sum_va_mixed(%tag: i32, ...) -> i32 cc "c" [nounwind] {
.entry:
    %ap = alloca.ptr 4, align 4
    va_start %ap
    %a = va_arg.i32 %ap
    %d = va_arg.f64 %ap
    %b = va_arg.i64 %ap
    %c = va_arg.i32 %ap
    va_end %ap
    %di = fptosi.i32 %d [sat]
    %bi = trunc.i32 %b
    %s0 = add.i32 %tag, %a
    %s1 = add.i32 %di, %bi
    %s2 = add.i32 %s0, %s1
    %r  = add.i32 %s2, %c
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Hardware sdiv/udiv, umull/smull, clz, ctz, popcnt, bswap
    %q = div_s.i32 -100000, 7        // -14285
    %r = rem_s.i32 -100000, 7        // -5
    %ml, %mh = mul_wide_u.i32 0x100000, 0x2000 // 0x200000000 -> lo=0, hi=2
    %vbit = const.i32 0x00123400
    %cz = clz.i32 %vbit              // 11
    %tz = ctz.i32 %vbit              // 10
    %pc = popcnt.i32 %vbit           // 5 (0001_0010_0011_0100 -> 1+1+2+1=5)
    %bs = bswap.i32 0x12345678       // 0x78563412
    %k1a = cmp_eq.i32 %q, -14285
    %k1b = cmp_eq.i32 %r, -5
    %k1c = cmp_eq.i32 %mh, 2
    %k1d = cmp_eq.i32 %cz, 11
    %k1e = cmp_eq.i32 %tz, 10
    %k1f = cmp_eq.i32 %pc, 5
    %k1g = cmp_eq.i32 %bs, 0x78563412
    %m1 = and.i8 %k1a, %k1b
    %m2 = and.i8 %k1c, %k1d
    %m3 = and.i8 %k1e, %k1f
    %m4 = and.i8 %m1, %m2
    %m5 = and.i8 %m3, %k1g
    %ok1 = and.i8 %m4, %m5
    br %ok1, .t2, .fail(1)

.t2:
    // 2. AAPCS32-VFP backfilling
    %bf = call @vfp_backfill(1.5, 2.25, 3.5, 4.75, 5.0) // 17.0
    %ok2 = fcmp_oeq.f64 %bf, 17.0
    br %ok2, .t3, .fail(2)

.t3:
    // 3. HFA4f & HFA3d returns and byval args
    %h4 = alloca @Hfa4f
    call @make_hfa4f(%h4, 1.0, 2.0, 3.0, 4.0)
    %sh4 = call @sum_hfa4f(5.0, 6.0, %h4)   // 21.0
    %h3 = alloca @Hfa3d
    call @make_hfa3d(%h3, 10.5, 20.5, 30.0)
    %sh3 = call @sum_hfa3d(%h3)              // 61.0
    %ok3a = fcmp_oeq.f32 %sh4, 21.0
    %ok3b = fcmp_oeq.f64 %sh3, 61.0
    %ok3 = and.i8 %ok3a, %ok3b
    br %ok3, .t4, .fail(3)

.t4:
    // 4. Non-HFA structs (Small3 in r0, Mid12 split r2/r3+stack, Big24 sret/indirect)
    %sm = alloca @Small3
    call @make_small3(%sm, 7, 300)
    %sp0 = field_ptr @Small3, %sm, 0
    %sp1 = field_ptr @Small3, %sm, 1
    %sv0 = load.i8 %sp0, align 1
    %sv1 = load.i16 %sp1, align 2
    %mid = alloca @Mid12
    %mp0 = field_ptr @Mid12, %mid, 0
    %mp1 = field_ptr @Mid12, %mid, 1
    %mp2 = field_ptr @Mid12, %mid, 2
    store.i32 10, %mp0, align 4
    store.i32 20, %mp1, align 4
    store.i32 30, %mp2, align 4
    %em = call @eval_mid12(1, 2, %mid)       // 63
    %big = alloca @Big24
    call @make_big24(%big, 10)
    %eb = call @sum_big24(%big)              // 10+11+12+13+14+15 = 75
    %ok4a = cmp_eq.i8 %sv0, 7
    %ok4b = cmp_eq.i16 %sv1, 300
    %ok4c = cmp_eq.i32 %em, 63
    %ok4d = cmp_eq.i32 %eb, 75
    %n1 = and.i8 %ok4a, %ok4b
    %n2 = and.i8 %ok4c, %ok4d
    %ok4 = and.i8 %n1, %n2
    br %ok4, .t5, .fail(4)

.t5:
    // 5. i64 even-register alignment
    %ia = const.i64 10000000000
    %ib = const.i64 23456789000
    %ir = call @i64_align_test(12, %ia, %ib)
    %ok5 = cmp_eq.i64 %ir, 33456789012
    br %ok5, .t6, .fail(5)

.t6:
    // 6. Thumb interworking function pointers (const.ptr & global table)
    %tbl = const.ptr @fn_tbl
    %f0 = load.ptr %tbl, align 4
    %tbl4 = ptr_add.i32 %tbl, 4
    %f1 = load.ptr %tbl4, align 4
    %ra = call_indirect @bin32, %f0(50, 17)
    %rs = call_indirect @bin32, %f1(50, 17)
    %ok6a = cmp_eq.i32 %ra, 67
    %ok6b = cmp_eq.i32 %rs, 33
    %ok6 = and.i8 %ok6a, %ok6b
    br %ok6, .t7, .fail(6)

.t7:
    // 7. Dense Thumb-2 jump table switch
    %sw1 = call @dense_sw(12)
    %sw2 = call @dense_sw(14)
    %sw3 = call @dense_sw(99)
    %ok7a = cmp_eq.i32 %sw1, 300
    %ok7b = cmp_eq.i32 %sw2, 500
    %ok7c = cmp_eq.i32 %sw3, -1
    %ok7ab = and.i8 %ok7a, %ok7b
    %ok7 = and.i8 %ok7ab, %ok7c
    br %ok7, .t8, .fail(7)

.t8:
    // 8. AAPCS32 variadics with i32, f64, i64, i32
    %va_a = const.i32 10
    %va_d = const.f64 25.0
    %va_b = const.i64 30
    %va_c = const.i32 40
    %vr = call @sum_va_mixed(5, %va_a, %va_d, %va_b, %va_c) // 5+10+25+30+40 = 110
    %ok8 = cmp_eq.i32 %vr, 110
    br %ok8, .t9, .fail(8)

.t9:
    // 9. Dynamic alloca_dyn + stacksave + stackrestore
    %sp = stacksave
    %sz = const.i32 32
    %dyn = alloca_dyn %sz, align 8
    store.i32 123456, %dyn, align 4
    %pend = ptr_add.i32 %dyn, 28
    store.i32 654321, %pend, align 4
    %lv0 = load.i32 %dyn, align 4
    %lv1 = load.i32 %pend, align 4
    stackrestore %sp
    %ok9a = cmp_eq.i32 %lv0, 123456
    %ok9b = cmp_eq.i32 %lv1, 654321
    %ok9 = and.i8 %ok9a, %ok9b
    br %ok9, .pass, .fail(9)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
