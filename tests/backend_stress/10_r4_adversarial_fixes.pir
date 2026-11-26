// Copyright (c) 2026 Ummon Karpe
// SPDX-License-Identifier: BSD-3-Clause
//
// Round 4 adversarial regression stress test for pirc:
//   1. Multiple switch.i128 in the same function (unique labels + cur_out UAF fix)
//   2. fptoui.i128 [trap] & fptosi.i128 [trap] (emit_bin FCMP_* -> i8 result type)
//   3. Single-rounding IEEE-754 uitofp/sitofp from i128 -> f64/f32
//   4. Braced global i128 = { zeroinit }
//   5. Scalar fptosi/fptoui [trap] exact trunc(src) lower bounds (-0.5, -128.5, -2147483648.5)
//   6. switch.i8 jump table with negative & wrapped cases (-128, -1, 0, 127)
//   7. bitcast.f32 / bitcast.f64 from immediate integer constants
//   8. Custom function Mach-O section marked executable (S_ATTR_PURE_INSTRUCTIONS)
//   9. multi_ret_slot >16-byte alignment (align 32 HFA return)
//  10. alias/sroa negative i32 immediate ptr_add offset
//  11. licm guarded alloca load not hoisted over uninitialized entry
//  12. simplify_cfg non-boolean select condition (cond = 2)
//  13. Vector fcmp + select preserving IEEE-754 -0.0 / +0.0 / NaN (no buggy fmin/fmax fold)

target "aarch64-apple-darwin"

type @Hfa32 = { f64, f64 }, align 32

@g_z128 = global i128 = { zeroinit }, align 16

func @custom_sec_fn(%x: i32) -> i32 cc "c" section "__TEXT,__pir_r4" [nounwind] {
.entry:
    %r = add.i32 %x, 41 [wrap]
    ret %r
}

func @multi_switch_i128(%a: i128, %b: i128) -> i32 cc "c" [nounwind] {
.entry:
    switch.i128 %a, default .a_def [
        0 -> .a_0,
        1 -> .a_1,
        0x10000000000000002 -> .a_big
    ]
.a_0:
    jmp .second(10)
.a_1:
    jmp .second(20)
.a_big:
    jmp .second(30)
.a_def:
    jmp .second(40)
.second(%acc: i32):
    switch.i128 %b, default .b_def [
        -1 -> .b_neg,
        0x20000000000000003 -> .b_big
    ]
.b_neg:
    %r1 = add.i32 %acc, 1 [wrap]
    ret %r1
.b_big:
    %r2 = add.i32 %acc, 2 [wrap]
    ret %r2
.b_def:
    %r3 = add.i32 %acc, 3 [wrap]
    ret %r3
}

func @i128_fp_round_and_trap() -> i32 cc "c" [nounwind] {
.entry:
    // Exact double-rounding trap for uitofp.f64:
    // ((2^53 + 1) << 10) + 1 = 0x2000000000000401 (exact midpoint + 1 ULP -> rounds UP!)
    // Double rounding (hi+lo) rounds to 0x2000000000000400 instead of 0x2000000000000800.
    // Use an i128 > 2^64: (1 << 64) + (1 << 10) + 1 = 0x10000000000000401
    // Midpoint between 2^64 and 2^64 + 2^11 is 2^64 + 2^10; +1 is strictly above midpoint!
    // Must round UP to 2^64 + 2^11 = 0x43f0000000000001 (bits).
    %c_hi = const.i128 0x10000000000000401
    %d_u = uitofp.f64 %c_hi
    %db = bitcast.i64 %d_u
    %ok1 = cmp_eq.i64 %db, 0x43f0000000000001

    // Same for sitofp.f64 on negative -(2^64 + 2^10 + 1) -> 0xc3f0000000000001
    %c_neg = sub.i128 0, %c_hi [wrap]
    %d_s = sitofp.f64 %c_neg
    %dsb = bitcast.i64 %d_s
    %ok2 = cmp_eq.i64 %dsb, 0xc3f0000000000001

    // fptoui.i128 [trap] and fptosi.i128 [trap]
    %f_pos = const.f64 12345.75
    %u128 = fptoui.i128 %f_pos [trap]
    %u64 = trunc.i64 %u128
    %ok3 = cmp_eq.i64 %u64, 12345

    %f_neg = const.f64 -54321.25
    %s128 = fptosi.i128 %f_neg [trap]
    %s64 = trunc.i64 %s128
    %ok4 = cmp_eq.i64 %s64, -54321

    // Check braced global i128 zeroinit
    %gz_p = addr @g_z128
    %gz = load.i128 %gz_p, align 16
    %ok5 = cmp_eq.i128 %gz, 0

    %a1 = and.i8 %ok1, %ok2
    %a2 = and.i8 %ok3, %ok4
    %a3 = and.i8 %a1, %a2
    %a4 = and.i8 %a3, %ok5
    %res = zext.i32 %a4
    ret %res
}

func @fptoi_trap_bounds(%f_mhalf: f64, %f_m128h: f64, %f_m32k: f64, %f_m2g: f64) -> i32 cc "c" [nounwind] {
.entry:
    // trunc(-0.5) == 0 -> valid for unsigned & signed [trap]
    %u8 = fptoui.i8 %f_mhalf [trap]
    %u32 = fptoui.i32 %f_mhalf [trap]
    %u64 = fptoui.i64 %f_mhalf [trap]
    // trunc(-128.5) == -128 -> valid for signed i8 [trap]
    %s8 = fptosi.i8 %f_m128h [trap]
    // trunc(-32768.5) == -32768 -> valid for signed i16 [trap]
    %s16 = fptosi.i16 %f_m32k [trap]
    // trunc(-2147483648.5) == -2147483648 -> valid for signed i32 [trap]
    %s32 = fptosi.i32 %f_m2g [trap]

    %ok1 = cmp_eq.i8 %u8, 0
    %ok2 = cmp_eq.i32 %u32, 0
    %ok3 = cmp_eq.i64 %u64, 0
    %ok4 = cmp_eq.i8 %s8, -128
    %ok5 = cmp_eq.i16 %s16, -32768
    %ok6 = cmp_eq.i32 %s32, -2147483648
    %b1 = and.i8 %ok1, %ok2
    %b2 = and.i8 %ok3, %ok4
    %b3 = and.i8 %ok5, %ok6
    %b4 = and.i8 %b1, %b2
    %b5 = and.i8 %b4, %b3
    %r = zext.i32 %b5
    ret %r
}

func @switch_i8_signed(%x: i8) -> i32 cc "c" [nounwind] {
.entry:
    switch.i8 %x, default .def [
        -128 -> .c_min,
        -1 -> .c_m1,
        0 -> .c_0,
        1 -> .c_1,
        127 -> .c_max
    ]
.c_min:
    ret 11
.c_m1:
    ret 22
.c_0:
    ret 33
.c_1:
    ret 44
.c_max:
    ret 55
.def:
    ret 99
}

func @bitcast_imm_test() -> i32 cc "c" [nounwind] {
.entry:
    %f1 = bitcast.f32 1065353216
    %d1 = bitcast.f64 4607182418800017408
    %ok1 = fcmp_oeq.f32 %f1, 1.0
    %ok2 = fcmp_oeq.f64 %d1, 1.0
    %ok = and.i8 %ok1, %ok2
    %r = zext.i32 %ok
    ret %r
}

func @make_hfa32(%out: retbuf(@Hfa32), %a: f64, %b: f64, %ok_ptr: ptr) -> void cc "c" [nounwind] {
.entry:
    %pi = ptrtoint.i64 %out
    %rem = and.i64 %pi, 31
    %aligned = cmp_eq.i64 %rem, 0
    %al32 = zext.i32 %aligned
    store.i32 %al32, %ok_ptr, align 4
    %p0 = field_ptr @Hfa32, %out, 0
    %p1 = field_ptr @Hfa32, %out, 1
    store.f64 %a, %p0, align 8
    store.f64 %b, %p1, align 8
    ret
}

func @neg_i32_ptr_add_alias() -> i32 cc "c" [nounwind] {
.entry:
    %arr = alloca [4 x i32], align 4
    %p0 = ptr_add.i64 %arr, 0
    %p1 = ptr_add.i64 %arr, 4
    %p2 = ptr_add.i64 %arr, 8
    store.i32 10, %p0, align 4
    store.i32 20, %p1, align 4
    store.i32 30, %p2, align 4
    // Negative i32 offset from &arr[2] -> &arr[1] (offset -4)
    %back = ptr_add.i32 %p2, -4
    store.i32 99, %back, align 4
    %v = load.i32 %p1, align 4
    ret %v
}

func @licm_guarded_uninit(%n: i32, %flag: i8) -> i32 cc "c" [nounwind] {
.entry:
    %slot = alloca i32, align 4
    jmp .loop(0, 0)
.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit, .body
.body:
    br %flag, .do_load, .skip
.do_load:
    store.i32 7, %slot, align 4
    %lv = load.i32 %slot, align 4
    %acc2 = add.i32 %acc, %lv [wrap]
    jmp .latch(%acc2)
.skip:
    jmp .latch(%acc)
.latch(%nacc: i32):
    %ni = add.i32 %i, 1 [wrap]
    jmp .loop(%ni, %nacc)
.exit:
    ret %acc
}

func @cfg_nonbool_cond(%c: i8, %a: i32, %b: i32) -> i32 cc "c" [nounwind] {
.entry:
    br %c, .t, .f
.t:
    jmp .join(%a)
.f:
    jmp .join(%b)
.join(%v: i32):
    ret %v
}

func @vec_fminmax_ieee() -> i32 cc "c" [nounwind] {
.entry:
    // Signed zero: -0.0 < +0.0 is false, so select(olt(-0.0, +0.0), -0.0, +0.0) MUST return +0.0 (bits 0x0)!
    %mz_i = const.i32 0x80000000
    %mz_f = bitcast.f32 %mz_i
    %pz_f = const.f32 0.0
    %va = splat.v4f32 %mz_f
    %vb = splat.v4f32 %pz_f
    %cm = fcmp_olt.v4f32 %va, %vb
    %sel = select.v4f32 %cm, %va, %vb
    %e0 = extract.v4f32 %sel, 0
    %bits = bitcast.i32 %e0
    %ok1 = cmp_eq.i32 %bits, 0

    // NaN: olt(NaN, 1.0) is false, so select(olt(NaN, 1.0), NaN, 1.0) MUST return 1.0 (not NaN)!
    %nan_i = const.i32 0x7fc00000
    %nan_f = bitcast.f32 %nan_i
    %one_f = const.f32 1.0
    %vn = splat.v4f32 %nan_f
    %v1 = splat.v4f32 %one_f
    %cm2 = fcmp_olt.v4f32 %vn, %v1
    %sel2 = select.v4f32 %cm2, %vn, %v1
    %e2 = extract.v4f32 %sel2, 0
    %ok2 = fcmp_oeq.f32 %e2, 1.0

    %ok = and.i8 %ok1, %ok2
    %r = zext.i32 %ok
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Custom executable function section
    %r1 = call @custom_sec_fn(1)
    %ok1 = cmp_eq.i32 %r1, 42
    br %ok1, .t2, .fail

.t2:
    // 2. Multiple switch.i128 in same function
    %r2a = call @multi_switch_i128(0x10000000000000002, 0x20000000000000003)
    %r2b = call @multi_switch_i128(1, -1)
    %s2 = add.i32 %r2a, %r2b [wrap]
    %ok2 = cmp_eq.i32 %s2, 53
    br %ok2, .t3, .fail

.t3:
    // 3. Single-rounding i128->fp + fptoi.i128 [trap] + braced zeroinit global
    %r3 = call @i128_fp_round_and_trap()
    %ok3 = cmp_eq.i32 %r3, 1
    br %ok3, .t4, .fail

.t4:
    // 4. Scalar fptoi [trap] lower bounds
    %r4 = call @fptoi_trap_bounds(-0.5, -128.5, -32768.5, -2147483648.5)
    %ok4 = cmp_eq.i32 %r4, 1
    br %ok4, .t5, .fail

.t5:
    // 5. switch.i8 negative/wrapped jump table
    %s5a = call @switch_i8_signed(-128)
    %s5b = call @switch_i8_signed(-1)
    %s5c = call @switch_i8_signed(127)
    %s5d = call @switch_i8_signed(42)
    %t5a = add.i32 %s5a, %s5b [wrap]
    %t5b = add.i32 %s5c, %s5d [wrap]
    %t5 = add.i32 %t5a, %t5b [wrap]
    %ok5 = cmp_eq.i32 %t5, 187
    br %ok5, .t6, .fail

.t6:
    // 6. bitcast from immediate integer
    %r6 = call @bitcast_imm_test()
    %ok6 = cmp_eq.i32 %r6, 1
    br %ok6, .t7, .fail

.t7:
    // 7. multi_ret_slot align 32 HFA return
    %hfa_buf = alloca @Hfa32, align 32
    %al_ok = alloca i32, align 4
    call @make_hfa32(%hfa_buf, 3.5, 4.5, %al_ok)
    %al_v = load.i32 %al_ok, align 4
    %hp0 = field_ptr @Hfa32, %hfa_buf, 0
    %hp1 = field_ptr @Hfa32, %hfa_buf, 1
    %hv0 = load.f64 %hp0, align 8
    %hv1 = load.f64 %hp1, align 8
    %hsum = fadd.f64 %hv0, %hv1
    %hok_s = fcmp_oeq.f64 %hsum, 8.0
    %hok_a = cmp_eq.i32 %al_v, 1
    %ok7 = and.i8 %hok_s, %hok_a
    br %ok7, .t8, .fail

.t8:
    // 8. Negative i32 ptr_add alias/sroa
    %r8 = call @neg_i32_ptr_add_alias()
    %ok8 = cmp_eq.i32 %r8, 99
    br %ok8, .t9, .fail

.t9:
    // 9. LICM guarded uninit alloca + SimplifyCFG non-boolean cond (2 -> true!)
    %r9a = call @licm_guarded_uninit(4, 0)
    %r9b = call @licm_guarded_uninit(4, 1)
    %r9c = call @cfg_nonbool_cond(2, 77, 13)
    %ok9a = cmp_eq.i32 %r9a, 0
    %ok9b = cmp_eq.i32 %r9b, 28
    %ok9c = cmp_eq.i32 %r9c, 77
    %ok9ab = and.i8 %ok9a, %ok9b
    %ok9 = and.i8 %ok9ab, %ok9c
    br %ok9, .t10, .fail

.t10:
    // 10. Vector IEEE-754 select(fcmp_olt)
    %r10 = call @vec_fminmax_ieee()
    %ok10 = cmp_eq.i32 %r10, 1
    br %ok10, .pass, .fail

.pass:
    ret 0
.fail:
    ret 1
}
