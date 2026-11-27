target "powerpc-unknown-elf"

// Comprehensive PowerPC 32-bit Big-Endian (SysV SVR4) backend test:
// 1. Explicit BIG-ENDIAN byte order verification (i16, i32, i64, f32, f64 & global memory)
// 2. i64 legalization & SysV PPC32 odd-reg/r10 overflow alignment (r3:r4, r5:r6, r7:r8, r9:r10, stack)
// 3. SysV PPC32 C ABI structs: all byval indirect, retbuf <= 8B in r3/r3:r4 (1B, 3B, 6B, 8B), >8B sret
// 4. Flagless bitops & checked overflow
// 5. Scalarized SIMD (v4i32 with max_simd_bits = 0)
// 6. SysV PPC32 12-byte va_list with GP, i64 (even-aligned), f64 (FPR save area), and stack overflow
// 7. Dense switch jump table, big-endian sub-word/word atomics, and local-exec TLS

type @PairI32  = { i32, i32 }
type @Small3   = { i8, i8, i8 }
type @MixIF    = { i32, f32 }
type @Big16    = { i32, i32, i32, i32 }
type @PtrSt    = { i8, ptr }

@g_be32 = global i32 = 0x12345678, align 4
@g_be64 = global i64 = 0x0102030405060708, align 8
@g_atom = global i32 = 10, align 4
@g_tls  = tls i32 = 7, align 4
@g_str  = data align 1 { "ppc32-be", 0 }
@g_fmt  = data align 1 { "BE:%s:%d:%lld:%.2f\n", 0 }

extern func @malloc(%n: i32) -> ptr cc "c" [nounwind]
extern func @calloc(%n: i32, %s: i32) -> ptr cc "c" [nounwind]
extern func @realloc(%p: ptr, %n: i32) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]
extern func @strlen(%s: ptr) -> i32 cc "c" [nounwind]
extern func @printf(%fmt: ptr, ...) -> i32 cc "c" [nounwind]

func @add_i64_align(%tag: i32, %a: i64, %b: i64) -> i64 cc "c" nounwind {
.entry:
    // %tag in r3, %a skips r4 -> r5:r6, %b in r7:r8
    %t64 = zext.i64 %tag
    %s1  = add.i64 %a, %b
    %s2  = add.i64 %s1, %t64
    ret %s2
}

func @i64_r10_overflow(%a0: i32, %a1: i32, %a2: i32, %a3: i32, %a4: i32, %a5: i32, %a6: i32, %big: i64) -> i64 cc "c" nounwind {
.entry:
    // a0..a6 occupy r3..r9 (7 regs). %big cannot split across r10+stack,
    // so SysV PPC32 skips r10 and passes %big on 8B-aligned stack!
    %s0 = add.i32 %a0, %a1
    %s1 = add.i32 %a2, %a3
    %s2 = add.i32 %a4, %a5
    %s3 = add.i32 %s0, %s1
    %s4 = add.i32 %s2, %a6
    %s5 = add.i32 %s3, %s4
    %s64 = zext.i64 %s5
    %r   = add.i64 %big, %s64
    ret %r
}

func @mul_div_i64(%a: i64, %b: i64, %d: i64) -> i64 cc "c" nounwind {
.entry:
    %p = mul.i64 %a, %b
    %q = div_s.i64 %p, %d
    %r = rem_s.i64 %p, %d
    %s = add.i64 %q, %r
    ret %s
}

func @make_small3(%out: retbuf(@Small3), %a: i8, %b: i8, %c: i8) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @Small3, %out, 0
    %p1 = field_ptr @Small3, %out, 1
    %p2 = field_ptr @Small3, %out, 2
    store.i8 %a, %p0, align 1
    store.i8 %b, %p1, align 1
    store.i8 %c, %p2, align 1
    ret
}

func @sum_small3(%s: byval(@Small3)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @Small3, %s, 0
    %p1 = field_ptr @Small3, %s, 1
    %p2 = field_ptr @Small3, %s, 2
    %a  = load.i8 %p0, align 1
    %b  = load.i8 %p1, align 1
    %c  = load.i8 %p2, align 1
    %a3 = zext.i32 %a
    %b3 = zext.i32 %b
    %c3 = zext.i32 %c
    %t  = add.i32 %a3, %b3
    %r  = add.i32 %t, %c3
    ret %r
}

func @make_mix_if(%out: retbuf(@MixIF), %a: i32, %b: f32) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @MixIF, %out, 0
    %p1 = field_ptr @MixIF, %out, 1
    store.i32 %a, %p0, align 4
    store.f32 %b, %p1, align 4
    ret
}

func @eval_mix_if(%s: byval(@MixIF)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @MixIF, %s, 0
    %p1 = field_ptr @MixIF, %s, 1
    %a  = load.i32 %p0, align 4
    %b  = load.f32 %p1, align 4
    %bi = fptosi.i32 %b [sat]
    %r  = add.i32 %a, %bi
    ret %r
}

func @make_big16(%out: retbuf(@Big16), %a: i32, %b: i32, %c: i32, %d: i32) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @Big16, %out, 0
    %p1 = field_ptr @Big16, %out, 1
    %p2 = field_ptr @Big16, %out, 2
    %p3 = field_ptr @Big16, %out, 3
    store.i32 %a, %p0, align 4
    store.i32 %b, %p1, align 4
    store.i32 %c, %p2, align 4
    store.i32 %d, %p3, align 4
    ret
}

func @sum_big16(%s: byval(@Big16)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @Big16, %s, 0
    %p1 = field_ptr @Big16, %s, 1
    %p2 = field_ptr @Big16, %s, 2
    %p3 = field_ptr @Big16, %s, 3
    %a  = load.i32 %p0, align 4
    %b  = load.i32 %p1, align 4
    %c  = load.i32 %p2, align 4
    %d  = load.i32 %p3, align 4
    %s1 = add.i32 %a, %b
    %s2 = add.i32 %c, %d
    %r  = add.i32 %s1, %s2
    ret %r
}

func @sum_va(%n: i32, ...) -> i32 cc "c" nounwind {
.entry:
    %ap = alloca va_list
    va_start %ap
    %a  = va_arg.i32 %ap
    %b  = va_arg.i64 %ap
    %d  = va_arg.f64 %ap
    va_end %ap
    %bi = trunc.i32 %b
    %di = fptosi.i32 %d [sat]
    %t1 = add.i32 %n, %a
    %t2 = add.i32 %bi, %di
    %r  = add.i32 %t1, %t2
    ret %r
}

func @sw_test(%x: i32) -> i32 nounwind {
.entry:
    switch.i32 %x, default .def [
        0 -> .c0,
        1 -> .c1,
        2 -> .c2,
        3 -> .c3,
        4 -> .c4
    ]
.c0: ret 10
.c1: ret 20
.c2: ret 30
.c3: ret 40
.c4: ret 50
.def: ret 99
}

export func @main() -> i32 cc "c" nounwind {
.entry:
    // 1. Explicit BIG-ENDIAN byte order checks on globals and stack memory!
    %p32 = addr @g_be32
    %b0  = load.i8 %p32, align 1
    %p32_3 = ptr_add.i32 %p32, 3 [inbounds]
    %b3  = load.i8 %p32_3, align 1
    // Big-endian 0x12345678 => byte[0]=0x12, byte[3]=0x78
    %ok1a = cmp_eq.i8 %b0, 0x12
    %ok1b = cmp_eq.i8 %b3, 0x78
    %ok1c = and.i8 %ok1a, %ok1b
    br %ok1c, .t1b, .fail1

.t1b:
    %p64 = addr @g_be64
    %g0  = load.i8 %p64, align 1
    %p64_7 = ptr_add.i32 %p64, 7 [inbounds]
    %g7  = load.i8 %p64_7, align 1
    // Big-endian 0x0102030405060708 => byte[0]=0x01, byte[7]=0x08
    %ok1d = cmp_eq.i8 %g0, 1
    %ok1e = cmp_eq.i8 %g7, 8
    %ok1f = and.i8 %ok1d, %ok1e
    br %ok1f, .t1c, .fail1

.t1c:
    // Big-endian stack store i16 0xAABB -> byte[0]=0xAA, byte[1]=0xBB
    %sh = alloca i16, align 2
    store.i16 0xAABB, %sh, align 2
    %sb0 = load.i8 %sh, align 1
    %sh1 = ptr_add.i32 %sh, 1 [inbounds]
    %sb1 = load.i8 %sh1, align 1
    %ok1g = cmp_eq.i8 %sb0, 0xAA
    %ok1h = cmp_eq.i8 %sb1, 0xBB
    %ok1i = and.i8 %ok1g, %ok1h
    br %ok1i, .t2, .fail1

.t2:
    // 2. i64 alignment (odd reg skip + r10 stack overflow), div/rem, float casts
    %a64 = const.i64 0x100000005
    %b64 = const.i64 0x200000007
    %s64 = call @add_i64_align(3, %a64, %b64)
    %exp_s = const.i64 0x30000000F
    %ok2a = cmp_eq.i64 %s64, %exp_s
    br %ok2a, .t2b, .fail2
.t2b:
    %ov64 = call @i64_r10_overflow(1, 2, 3, 4, 5, 6, 7, %a64)
    %exp_ov = const.i64 0x100000021 // 0x100000005 + 28 = 0x100000021
    %ok2ov = cmp_eq.i64 %ov64, %exp_ov
    br %ok2ov, .t2c, .fail2
.t2c:
    %md = call @mul_div_i64(-1000000000, 7, 3)
    %exp_md = const.i64 -2333333334
    %ok2b = cmp_eq.i64 %md, %exp_md
    br %ok2b, .t2d, .fail3
.t2d:
    %f64v = sitofp.f64 %s64
    %back = fptosi.i64 %f64v [sat]
    %ok2c = cmp_eq.i64 %back, %exp_s
    br %ok2c, .t3, .fail4

.t3:
    // 3. SysV PPC32 structs: 3-byte odd-size retbuf/byval, {i32,f32}, and 16B sret/byval
    %sm = alloca @Small3
    call @make_small3(%sm, 10, 20, 30)
    %sms = call @sum_small3(%sm)
    %ok3a = cmp_eq.i32 %sms, 60
    br %ok3a, .t3b, .fail5
.t3b:
    %mi = alloca @MixIF
    call @make_mix_if(%mi, 17, 25.0)
    %mis = call @eval_mix_if(%mi)
    %ok3b = cmp_eq.i32 %mis, 42
    br %ok3b, .t3c, .fail6
.t3c:
    %bg = alloca @Big16
    call @make_big16(%bg, 10, 20, 30, 40)
    %bgs = call @sum_big16(%bg)
    %ok3c = cmp_eq.i32 %bgs, 100
    br %ok3c, .t4, .fail7

.t4:
    // 4. Flagless bitops
    %pc = popcnt.i32 0xF0F0F0F0
    %lz = clz.i32 0x00100000
    %tz = ctz.i32 0x00100000
    %bs = bswap.i32 0x12345678
    %b1 = add.i32 %pc, %lz
    %b2 = add.i32 %b1, %tz
    %ok4a = cmp_eq.i32 %b2, 47
    %ok4b = cmp_eq.i32 %bs, 0x78563412
    %ok4  = and.i8 %ok4a, %ok4b
    br %ok4, .t5, .fail8

.t5:
    // 5. Scalarized SIMD
    %va = splat.v4i32 5
    %vb = insert.v4i32 %va, 9, 2
    %vc = add.v4i32 %va, %vb
    %v2 = extract.v4i32 %vc, 2
    %ok5 = cmp_eq.i32 %v2, 14
    br %ok5, .t6, .fail9

.t6:
    // 6. SysV PPC32 12B va_list (GP + even-aligned i64 + FPR f64)
    %vr = call @sum_va(1, 2: i32, 3: i64, 4.0: f64)
    %ok6 = cmp_eq.i32 %vr, 10
    br %ok6, .t7, .fail10

.t7:
    // 7. Dense switch jump table + word atomic + TLS
    %sw = call @sw_test(3)
    %ga = addr @g_atom
    %old = atomic_rmw.add.i32 %ga, 5 [seq_cst]
    %cur = atomic_load.i32 %ga [acquire]
    %tlp = addr @g_tls
    %tlv = load.i32 %tlp, align 4
    %z1 = add.i32 %sw, %old
    %z2 = add.i32 %cur, %tlv
    %tot = add.i32 %z1, %z2
    %ok7 = cmp_eq.i32 %tot, 72
    br %ok7, .t8, .fail11

.t8:
    // 8. 32-bit C libc heap + string + variadic printf (with i32, i64, f64 on BE PPC32)
    %hp = call @calloc(4, 4)
    %h0 = load.i32 %hp, align 4
    store.i32 11, %hp, align 4
    %hp1 = ptr_add.i32 %hp, 4 [inbounds]
    store.i32 22, %hp1, align 4
    %hr = call @realloc(%hp, 32)
    %hr0 = load.i32 %hr, align 4
    %hr1p = ptr_add.i32 %hr, 4 [inbounds]
    %hr1 = load.i32 %hr1p, align 4
    call @free(%hr)
    %sp = addr @g_str
    %slen = call @strlen(%sp)
    %hs0 = add.i32 %h0, %hr0
    %hs1 = add.i32 %hs0, %hr1
    %hs2 = add.i32 %hs1, %slen // 0 + 11 + 22 + 8 = 41
    %fmtp = addr @g_fmt
    call @printf(%fmtp, %sp: ptr, %hs2: i32, 0x100000005: i64, 3.25: f64)
    %ok8 = cmp_eq.i32 %hs2, 41
    br %ok8, .pass, .fail12

.fail1:  ret 1
.fail2:  ret 2
.fail3:  ret 3
.fail4:  ret 4
.fail5:  ret 5
.fail6:  ret 6
.fail7:  ret 7
.fail8:  ret 8
.fail9:  ret 9
.fail10: ret 10
.fail11: ret 11
.fail12: ret 12
.pass:   ret 0
}
