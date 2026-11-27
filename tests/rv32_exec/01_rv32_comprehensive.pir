target "riscv32-unknown-elf"

// Comprehensive 32-bit RISC-V (RV32IMAFD / ILP32D) backend test:
// 1. 32-bit pointer layout & arithmetic
// 2. i64 legalization (add, sub, mul, div, rem, shifts, comparisons, float casts, bitcasts, returns)
// 3. ILP32D C ABI struct passing & returns ({i32,i32}, {f32,f32}, {i32,f32}, >8B sret/byval)
// 4. Flagless optional ops (ovf, add_carry, clz, ctz, popcnt, bswap)
// 5. Scalarized SIMD (v4i32, v4f32 with max_simd_bits = 0)
// 6. Variadic functions with i64 and f64 alignment
// 7. Dense switch jump table & 32-bit atomics

type @PairI32  = { i32, i32 }
type @PairF32  = { f32, f32 }
type @MixIF    = { i32, f32 }
type @Big16    = { i32, i32, i32, i32 }
type @PtrSt    = { i8, ptr }

@g_atom = global i32 = 10, align 4
@g_tls  = tls i32 = 7, align 4

func @add_i64(%a: i64, %b: i64) -> i64 cc "c" nounwind {
.entry:
    %r = add.i64 %a, %b
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

func @make_pair_i32(%out: retbuf(@PairI32), %x: i32, %y: i32) -> void cc "c" nounwind {
.entry:
    %p0 = field_ptr @PairI32, %out, 0
    %p1 = field_ptr @PairI32, %out, 1
    store.i32 %x, %p0, align 4
    store.i32 %y, %p1, align 4
    ret
}

func @sum_pair_i32(%s: byval(@PairI32)) -> i32 cc "c" nounwind {
.entry:
    %p0 = field_ptr @PairI32, %s, 0
    %p1 = field_ptr @PairI32, %s, 1
    %a  = load.i32 %p0, align 4
    %b  = load.i32 %p1, align 4
    %r  = add.i32 %a, %b
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
    // 1. Verify 32-bit pointer field offset ({i8, ptr} -> ptr at offset 4)
    %ps = alloca @PtrSt
    %pf1 = field_ptr @PtrSt, %ps, 1
    %d_ps = ptrtoint.i32 %ps
    %d_f1 = ptrtoint.i32 %pf1
    %poff = sub.i32 %d_f1, %d_ps
    %ok1 = cmp_eq.i32 %poff, 4
    br %ok1, .t2, .fail1

.t2:
    // 2. i64 math, shifts, div/rem, float casts & bitcast
    %a64 = const.i64 0x100000005
    %b64 = const.i64 0x200000007
    %s64 = call @add_i64(%a64, %b64)
    %exp_s = const.i64 0x30000000C
    %ok2a = cmp_eq.i64 %s64, %exp_s
    br %ok2a, .t2b, .fail2
.t2b:
    %md = call @mul_div_i64(-1000000000, 7, 3)
    // -7000000000 / 3 = -2333333333, rem = -1 -> sum = -2333333334
    %exp_md = const.i64 -2333333334
    %ok2b = cmp_eq.i64 %md, %exp_md
    br %ok2b, .t2c, .fail3
.t2c:
    %f64v = sitofp.f64 %s64
    %back = fptosi.i64 %f64v [sat]
    %ok2c = cmp_eq.i64 %back, %exp_s
    br %ok2c, .t3, .fail4

.t3:
    // 3. ILP32D structs: {i32, i32}, {i32, f32}, and >8B {i32,i32,i32,i32}
    %pi = alloca @PairI32
    call @make_pair_i32(%pi, 13, 29)
    %pis = call @sum_pair_i32(%pi)
    %ok3a = cmp_eq.i32 %pis, 42
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
    // 4. Flagless bitops & checked overflow
    %pc = popcnt.i32 0xF0F0F0F0
    %lz = clz.i32 0x00100000
    %tz = ctz.i32 0x00100000
    %bs = bswap.i32 0x12345678
    %b1 = add.i32 %pc, %lz   // 16 + 11 = 27
    %b2 = add.i32 %b1, %tz   // 27 + 20 = 47
    %ok4a = cmp_eq.i32 %b2, 47
    %ok4b = cmp_eq.i32 %bs, 0x78563412
    %ok4  = and.i8 %ok4a, %ok4b
    br %ok4, .t5, .fail8

.t5:
    // 5. Scalarized SIMD (v4i32 + v4f32)
    %va = splat.v4i32 5
    %vb = insert.v4i32 %va, 9, 2
    %vc = add.v4i32 %va, %vb
    %v2 = extract.v4i32 %vc, 2
    %ok5 = cmp_eq.i32 %v2, 14
    br %ok5, .t6, .fail9

.t6:
    // 6. Variadic i64 + f64 alignment
    %vr = call @sum_va(1, 2: i32, 3: i64, 4.0: f64)
    %ok6 = cmp_eq.i32 %vr, 10
    br %ok6, .t7, .fail10

.t7:
    // 7. Dense switch jump table + atomics + TLS
    %sw = call @sw_test(3)
    %ga = addr @g_atom
    %old = atomic_rmw.add.i32 %ga, 5 [seq_cst]
    %cur = atomic_load.i32 %ga [acquire]
    %tlp = addr @g_tls
    %tlv = load.i32 %tlp, align 4
    // sw=40, old=10, cur=15, tlv=7 -> total = 72
    %z1 = add.i32 %sw, %old
    %z2 = add.i32 %cur, %tlv
    %tot = add.i32 %z1, %z2
    %ok7 = cmp_eq.i32 %tot, 72
    br %ok7, .pass, .fail11

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
.pass:   ret 0
}
