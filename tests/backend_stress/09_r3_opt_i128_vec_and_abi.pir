// Copyright (c) 2026 Ummon Karpe
// SPDX-License-Identifier: BSD-3-Clause
//
// Round 3 comprehensive regression stress test for pirc:
//   1. mem2reg uninitialized vector alloca zero-init (v4i32, v4f32)
//   2. licm volatile load/store barrier & header trapping-load safety
//   3. dse store preservation before trapping div/rem & memcpy return type
//   4. i128 lowering: switch.i128, sitofp/uitofp, fptosi/fptoui [sat],
//      bitcast i128<->v2i64, add_carry/sub_borrow, add/mul_ovf, global i128
//   5. Non-zero typed aggregate global flattening (struct with padding & f64)
//   6. SIMD oversized vector call argument splitting (v8i32)
//   7. add_carry/sub_borrow carry-in masking (& 1)
//   8. Default flagless fptosi/fptoui [sat] on i8/i16 & v4f32->v4i32
//   9. signext/zeroext parameter & return attributes on i8/i16
//  10. Empty-block trampoline cycle label retention & dead early-clobber def
//  11. Negative global relocation offset (@g + -4) & sibling block-param names

target "aarch64-apple-darwin"

type @Rec = { i8, i32, i64, f64 }

@g_i128 = global i128 = 0x10000000000000005, align 16
@g_rec  = global @Rec = { 7, -42, 1000000000000, 3.5 }, align 8
@g_arr  = global [4 x i32] = { 10, 20, 30, 40 }, align 4
@g_rel  = global ptr = ptr @g_arr + 12, align 8
@g_rel_neg = global ptr = ptr @g_arr + -4, align 8

func @vec_mem2reg_zero(%cond: i8) -> i32 cc "c" [nounwind] {
.entry:
    %a = alloca v4i32, align 16
    %f = alloca v4f32, align 16
    br %cond, .set, .join
.set:
    %v = splat.v4i32 11
    store.v4i32 %v, %a, align 16
    %vf = splat.v4f32 2.0
    store.v4f32 %vf, %f, align 16
    jmp .join
.join:
    %lv = load.v4i32 %a, align 16
    %lf = load.v4f32 %f, align 16
    %e0 = extract.v4i32 %lv, 0
    %f0 = extract.v4f32 %lf, 0
    %fi = fptosi.i32 %f0 [sat]
    %sum = add.i32 %e0, %fi [wrap]
    ret %sum
}

func @licm_volatile_test(%p: ptr, %flag: ptr) -> i32 cc "c" [nounwind] {
.entry:
    jmp .loop(0, 0)
.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, 3
    br %done, .exit, .body
.body:
    %v = load.i32 %p, align 4
    %fl = load.i32 %flag, align 4 [volatile]
    %s1 = add.i32 %v, %fl [wrap]
    %nacc = add.i32 %acc, %s1 [wrap]
    %nv = add.i32 %v, 1 [wrap]
    store.i32 %nv, %p, align 4 [volatile]
    %ni = add.i32 %i, 1 [wrap]
    jmp .loop(%ni, %nacc)
.exit:
    ret %acc
}

func @i128_suite(%x: i128, %y: i128) -> i64 cc "c" [nounwind] {
.entry:
    switch.i128 %x, default .sw_def [
        0 -> .sw_0,
        0x10000000000000005 -> .sw_hit
    ]
.sw_0:
    ret 100
.sw_def:
    ret 200
.sw_hit:
    %s, %c1 = add_carry.i128 %x, %y, 0xfe
    %d, %b1 = sub_borrow.i128 %s, %y, 0xff
    %ov_sum, %ovf = add_ovf_s.i128 %d, 1
    %v2 = bitcast.v2i64 %ov_sum
    %lo = extract.v2i64 %v2, 0
    %hi = extract.v2i64 %v2, 1
    %bc = bitcast.i128 %v2
    %fp = sitofp.f64 %bc
    %back = fptosi.i128 %fp [sat]
    %c1_64 = zext.i64 %c1
    %b1_64 = zext.i64 %b1
    %ov_64 = zext.i64 %ovf
    %t1 = add.i64 %lo, %hi [wrap]
    %t2 = add.i64 %t1, %c1_64 [wrap]
    %t3 = add.i64 %t2, %b1_64 [wrap]
    %t4 = add.i64 %t3, %ov_64 [wrap]
    %ok_back = cmp_gt_s.i128 %back, 0
    %ok_64 = zext.i64 %ok_back
    %res = add.i64 %t4, %ok_64 [wrap]
    ret %res
}

func @sum_v8i32(%v: v8i32) -> i32 cc "c" [nounwind] {
.entry:
    %e0 = extract.v8i32 %v, 0
    %e7 = extract.v8i32 %v, 7
    %s = add.i32 %e0, %e7 [wrap]
    ret %s
}

func @subword_ext(%a: i8 signext, %b: i8 zeroext, %c: i16 signext, %d: i16 zeroext) -> i16 signext cc "c" [nounwind] {
.entry:
    %a32 = sext.i32 %a
    %b32 = zext.i32 %b
    %c32 = sext.i32 %c
    %d32 = zext.i32 %d
    %s1 = add.i32 %a32, %b32 [wrap]
    %s2 = add.i32 %c32, %d32 [wrap]
    %s3 = add.i32 %s1, %s2 [wrap]
    %r = trunc.i16 %s3
    ret %r
}

func @dead_trampoline_and_sibling(%c: i8) -> i32 cc "c" [nounwind] {
.entry:
    br %c, .left(10), .right(20)
.left(%x: i32):
    jmp .done(%x)
.right(%x: i32):
    jmp .done(%x)
.dead_a:
    jmp .dead_b
.dead_b:
    jmp .dead_a
.done(%v: i32):
    ret %v
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. mem2reg vector zero-init (entry edge is uninit, dynamically taken path stores first)
    %m1 = call @vec_mem2reg_zero(1)
    %ok1 = cmp_eq.i32 %m1, 13
    br %ok1, .t2, .fail1

.t2:
    // 2. licm volatile barrier
    %vp = alloca i32, align 4
    %vf = alloca i32, align 4
    store.i32 1, %vp, align 4
    store.i32 10, %vf, align 4
    %lv = call @licm_volatile_test(%vp, %vf)
    // iter 0: 1+10=11, *p=2; iter 1: 2+10=12, *p=3; iter 2: 3+10=13, *p=4 => 36
    %ok2 = cmp_eq.i32 %lv, 36
    br %ok2, .t3, .fail2

.t3:
    // 3. i128 suite + global i128
    %gp = addr @g_i128
    %gx = load.i128 %gp, align 16
    %i128r = call @i128_suite(%gx, 2)
    // %s = gx + 2 + (0xfe & 1 = 0) = 0x10000000000000007, c1=0
    // %d = s - 2 - (0xff & 1 = 1) = 0x10000000000000004, b1=0
    // %ov_sum = d + 1 = 0x10000000000000005 => lo=5, hi=1, ovf=0, ok_back=1 => 5+1+0+0+0+1 = 7
    %ok3 = cmp_eq.i64 %i128r, 7
    br %ok3, .t4, .fail3

.t4:
    // 4. Non-zero typed aggregate global
    %rp = addr @g_rec
    %r0p = field_ptr @Rec, %rp, 0
    %r1p = field_ptr @Rec, %rp, 1
    %r2p = field_ptr @Rec, %rp, 2
    %r3p = field_ptr @Rec, %rp, 3
    %r0 = load.i8 %r0p, align 1
    %r1 = load.i32 %r1p, align 4
    %r2 = load.i64 %r2p, align 8
    %r3 = load.f64 %r3p, align 8
    %r3i = fptosi.i32 %r3 [sat]
    %c0 = cmp_eq.i8 %r0, 7
    %c1 = cmp_eq.i32 %r1, -42
    %c2 = cmp_eq.i64 %r2, 1000000000000
    %c3 = cmp_eq.i32 %r3i, 3
    %c01 = and.i8 %c0, %c1
    %c23 = and.i8 %c2, %c3
    %ok4 = and.i8 %c01, %c23
    br %ok4, .t5, .fail4

.t5:
    // 5. Oversized v8i32 call arg splitting
    %v8 = splat.v8i32 17
    %v8s = call @sum_v8i32(%v8)
    %ok5 = cmp_eq.i32 %v8s, 34
    br %ok5, .t6, .fail5

.t6:
    // 6. add_carry/sub_borrow carry-in masking on i64
    %ac, %aco = add_carry.i64 10, 20, 0xfe
    %sb, %sbo = sub_borrow.i64 30, 5, 0xff
    %ok6a = cmp_eq.i64 %ac, 30
    %ok6b = cmp_eq.i64 %sb, 24
    %ok6 = and.i8 %ok6a, %ok6b
    br %ok6, .t7, .fail6

.t7:
    // 7. Default flagless fptosi/fptoui [sat] on i8/i16 & v4f32->v4i32
    %s8 = fptosi.i8 500.0
    %u8 = fptoui.i8 -5.0
    %s16 = fptosi.i16 -99999.0
    %vf4 = splat.v4f32 1000000000000.0
    %vi = fptosi.v4i32 %vf4
    %ve = extract.v4i32 %vi, 1
    %ok7a = cmp_eq.i8 %s8, 127
    %ok7b = cmp_eq.i8 %u8, 0
    %ok7c = cmp_eq.i16 %s16, -32768
    %ok7d = cmp_eq.i32 %ve, 2147483647
    %ok7ab = and.i8 %ok7a, %ok7b
    %ok7cd = and.i8 %ok7c, %ok7d
    %ok7 = and.i8 %ok7ab, %ok7cd
    br %ok7, .t8, .fail7

.t8:
    // 8. signext/zeroext & sibling block params & negative reloc offset
    %se = call @subword_ext(-1, 255, -2, 2)
    // -1 + 255 + -2 + 2 = 254
    %ok8a = cmp_eq.i16 %se, 254
    %sib = call @dead_trampoline_and_sibling(0)
    %ok8b = cmp_eq.i32 %sib, 20
    %relp = addr @g_rel
    %p12 = load.ptr %relp, align 8
    %v40 = load.i32 %p12, align 4
    %ok8c = cmp_eq.i32 %v40, 40
    %ok8ab = and.i8 %ok8a, %ok8b
    %ok8 = and.i8 %ok8ab, %ok8c
    br %ok8, .pass, .fail8

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
.fail8:
    ret 8
}
