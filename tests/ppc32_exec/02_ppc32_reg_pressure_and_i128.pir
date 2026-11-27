target "powerpc-unknown-elf"

// High register pressure (GP + FP spilling), >8 GP/FP call args,
// alloca_dyn, i128 arithmetic/div/rem/shifts, and big-endian sub-word + word + 64-bit atomics.

func @sum_12_i32(%a0: i32, %a1: i32, %a2: i32, %a3: i32, %a4: i32, %a5: i32, %a6: i32, %a7: i32, %a8: i32, %a9: i32, %a10: i32, %a11: i32) -> i32 cc "c" nounwind {
.entry:
    %s0 = add.i32 %a0, %a1
    %s1 = add.i32 %a2, %a3
    %s2 = add.i32 %a4, %a5
    %s3 = add.i32 %a6, %a7
    %s4 = add.i32 %a8, %a9
    %s5 = add.i32 %a10, %a11
    %t0 = add.i32 %s0, %s1
    %t1 = add.i32 %s2, %s3
    %t2 = add.i32 %s4, %s5
    %u0 = add.i32 %t0, %t1
    %u1 = add.i32 %u0, %t2
    ret %u1
}

func @sum_10_f64(%f0: f64, %f1: f64, %f2: f64, %f3: f64, %f4: f64, %f5: f64, %f6: f64, %f7: f64, %f8: f64, %f9: f64) -> f64 cc "c" nounwind {
.entry:
    %a = fadd.f64 %f0, %f1
    %b = fadd.f64 %f2, %f3
    %c = fadd.f64 %f4, %f5
    %d = fadd.f64 %f6, %f7
    %e = fadd.f64 %f8, %f9
    %ab = fadd.f64 %a, %b
    %cd = fadd.f64 %c, %d
    %abcd = fadd.f64 %ab, %cd
    %r = fadd.f64 %abcd, %e
    ret %r
}

func @test_gp_spill_and_atomics(%base: i32) -> i32 nounwind {
.entry:
    %v00 = add.i32 %base, 1
    %v01 = add.i32 %base, 2
    %v02 = add.i32 %base, 3
    %v03 = add.i32 %base, 4
    %v04 = add.i32 %base, 5
    %v05 = add.i32 %base, 6
    %v06 = add.i32 %base, 7
    %v07 = add.i32 %base, 8
    %v08 = add.i32 %base, 9
    %v09 = add.i32 %base, 10
    %v10 = add.i32 %base, 11
    %v11 = add.i32 %base, 12
    %v12 = add.i32 %base, 13
    %v13 = add.i32 %base, 14
    %v14 = add.i32 %base, 15
    %v15 = add.i32 %base, 16
    %v16 = add.i32 %base, 17
    %v17 = add.i32 %base, 18
    %v18 = add.i32 %base, 19
    %v19 = add.i32 %base, 20
    %v20 = add.i32 %base, 21
    %v21 = add.i32 %base, 22
    %v22 = add.i32 %base, 23
    %v23 = add.i32 %base, 24
    %v24 = add.i32 %base, 25
    %v25 = add.i32 %base, 26

    %csum = call @sum_12_i32(%v00, %v01, %v02, %v03, %v04, %v05, %v06, %v07, %v08, %v09, %v10, %v11)

    // Sub-word (i8 at byte offset 1, i16 at byte offset 2) big-endian atomics + 64-bit atomics
    %ab = alloca i32, align 4
    store.i32 0, %ab, align 4
    %ab1 = ptr_add.i32 %ab, 1 [inbounds]
    store.i8 10, %ab1, align 1
    %old8 = atomic_rmw.add.i8 %ab1, 5 [seq_cst]
    %cas8_old, %cas8_ok = atomic_cmpxchg.i8 %ab1, 15, 25 [seq_cst, seq_cst]
    %b1 = load.i8 %ab1, align 1
    %b1_32 = zext.i32 %b1
    %old8_32 = zext.i32 %old8
    %cas8_old_32 = zext.i32 %cas8_old
    %cas8_ok_32 = zext.i32 %cas8_ok

    %ah = alloca i32, align 4
    store.i32 0, %ah, align 4
    %ah2 = ptr_add.i32 %ah, 2 [inbounds]
    store.i16 100, %ah2, align 2
    %old16 = atomic_rmw.sub.i16 %ah2, 30 [seq_cst]
    %cas16_old, %cas16_ok = atomic_cmpxchg.i16 %ah2, 70, 90 [seq_cst, seq_cst]
    %h2 = load.i16 %ah2, align 2
    %h2_32 = zext.i32 %h2
    %old16_32 = zext.i32 %old16
    %cas16_old_32 = zext.i32 %cas16_old
    %cas16_ok_32 = zext.i32 %cas16_ok

    %a64 = alloca i64, align 8
    %k1 = const.i64 1000000000000
    %k2 = const.i64 500
    %k3 = const.i64 1000000000500
    %k4 = const.i64 1000000000777
    atomic_store.i64 %k1, %a64 [seq_cst]
    %o64 = atomic_rmw.add.i64 %a64, %k2 [seq_cst]
    %c64_o, %c64_ok = atomic_cmpxchg.i64 %a64, %k3, %k4 [seq_cst, seq_cst]
    %v64 = atomic_load.i64 %a64 [seq_cst]
    %d64 = sub.i64 %v64, %o64
    %d64_32 = trunc.i32 %d64
    %c64_ok_32 = zext.i32 %c64_ok

    %p0 = add.i32 %v00, %v01
    %p1 = add.i32 %v02, %v03
    %p2 = add.i32 %v04, %v05
    %p3 = add.i32 %v06, %v07
    %p4 = add.i32 %v08, %v09
    %p5 = add.i32 %v10, %v11
    %p6 = add.i32 %v12, %v13
    %p7 = add.i32 %v14, %v15
    %p8 = add.i32 %v16, %v17
    %p9 = add.i32 %v18, %v19
    %pa = add.i32 %v20, %v21
    %pb = add.i32 %v22, %v23
    %pc = add.i32 %v24, %v25

    %q0 = add.i32 %p0, %p1
    %q1 = add.i32 %p2, %p3
    %q2 = add.i32 %p4, %p5
    %q3 = add.i32 %p6, %p7
    %q4 = add.i32 %p8, %p9
    %q5 = add.i32 %pa, %pb
    %q6 = add.i32 %q0, %q1
    %q7 = add.i32 %q2, %q3
    %q8 = add.i32 %q4, %q5
    %q9 = add.i32 %q6, %q7
    %qa = add.i32 %q8, %pc
    %all_v = add.i32 %q9, %qa

    %a8_sum = add.i32 %old8_32, %cas8_old_32
    %a8_sum2 = add.i32 %a8_sum, %cas8_ok_32
    %a8_sum3 = add.i32 %a8_sum2, %b1_32

    %a16_sum = add.i32 %old16_32, %cas16_old_32
    %a16_sum2 = add.i32 %a16_sum, %cas16_ok_32
    %a16_sum3 = add.i32 %a16_sum2, %h2_32

    %r0 = add.i32 %all_v, %csum
    %r1 = add.i32 %r0, %a8_sum3
    %r2 = add.i32 %r1, %a16_sum3
    %r3 = add.i32 %r2, %d64_32
    %r4 = add.i32 %r3, %c64_ok_32
    ret %r4
}

func @test_fp_spill_and_alloca_dyn(%n: i32) -> i32 nounwind {
.entry:
    %buf = alloca_dyn %n, align 16
    store.i32 42, %buf, align 4

    %f0  = const.f64 1.0
    %f1  = const.f64 2.0
    %f2  = const.f64 3.0
    %f3  = const.f64 4.0
    %f4  = const.f64 5.0
    %f5  = const.f64 6.0
    %f6  = const.f64 7.0
    %f7  = const.f64 8.0
    %f8  = const.f64 9.0
    %f9  = const.f64 10.0
    %f10 = const.f64 11.0
    %f11 = const.f64 12.0
    %f12 = const.f64 13.0
    %f13 = const.f64 14.0

    %s = call @sum_10_f64(%f0, %f1, %f2, %f3, %f4, %f5, %f6, %f7, %f8, %f9)
    %t0 = fadd.f64 %f10, %f11
    %t1 = fadd.f64 %f12, %f13
    %t2 = fadd.f64 %t0, %t1
    %tot = fadd.f64 %s, %t2
    %ti = fptosi.i32 %tot [sat]
    %b0 = load.i32 %buf, align 4
    %res = add.i32 %ti, %b0
    ret %res
}

func @test_i128(%a: i64, %b: i64) -> i32 nounwind {
.entry:
    %x = zext.i128 %a
    %y = zext.i128 %b
    %c40 = const.i128 40
    %xs = shl.i128 %x, %c40
    %sum = add.i128 %xs, %y
    %prod = mul.i128 %x, %y
    %q = div_u.i128 %prod, %y
    %r = rem_u.i128 %sum, %y
    %ok1 = cmp_eq.i128 %q, %x
    %ok1_32 = zext.i32 %ok1
    %r32 = trunc.i32 %r
    %out = add.i32 %r32, %ok1_32
    ret %out
}

export func @main() -> i32 cc "c" {
.entry:
    %r1 = call @test_gp_spill_and_atomics(1)
    %c1 = cmp_eq.i32 %r1, 1557
    br %c1, .ok1, .fail1
.fail1:
    ret 1

.ok1:
    %r2 = call @test_fp_spill_and_alloca_dyn(64)
    %c2 = cmp_eq.i32 %r2, 147
    br %c2, .ok2, .fail2
.fail2:
    ret 2

.ok2:
    %r3 = call @test_i128(1000, 7)
    %c3 = cmp_eq.i32 %r3, 6
    br %c3, .ok3, .fail3
.fail3:
    ret 3

.ok3:
    ret 0
}
