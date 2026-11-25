// Copyright (c) 2026 Ummon Karpe
// SPDX-License-Identifier: BSD-3-Clause
//
// Regression tests for:
//   1. x86-64 PIR_OP_MUL_OVF_U with dead overflow flag (must not clobber RAX product)
//   2. x86-64 vector div_s/rem_s [wrap] on INT32_MIN / -1 (must wrap, not SIGFPE)
//   3. ARM64 vector fptosi/fptoui sub-32b [sat] clamping (v4f32 -> v4i16 [sat])
//   4. SIMD legalization cross-width vector casts (v4i64 <-> v4i32, v4f64 <-> v4f32)
//   5. ARM64 vector lane-wise cast initialization (no uninit acc vreg)

target "aarch64-apple-darwin"

func @mul_dead_ovf(%a: i64, %b: i64) -> i64 cc "c" [nounwind] {
.entry:
    %prod, %ovf = mul_ovf_u.i64 %a, %b
    ret %prod
}

func @vec_cast_uninit(%v: v2i32, %a: f64, %b: f64, %c: f64, %d: f64) -> v2i64 cc "c" [nounwind] {
.entry:
    %r = sext.v2i64 %v
    ret %r
}

func @cross_width_casts(%p_i64: ptr, %p_f64: ptr) -> i64 cc "c" [nounwind] {
.entry:
    %v64 = load.v4i64 %p_i64, align 8
    %v32 = trunc.v4i32 %v64
    %w64 = sext.v4i64 %v32
    %e0  = extract.v4i64 %w64, 0
    %e3  = extract.v4i64 %w64, 3

    %vf64 = load.v4f64 %p_f64, align 8
    %vf32 = fptrunc.v4f32 %vf64
    %wf64 = fpext.v4f64 %vf32
    %fe2  = extract.v4f64 %wf64, 2
    %fi2  = fptosi.i64 %fe2 [sat]

    %s0 = add.i64 %e0, %e3 [wrap]
    %s1 = add.i64 %s0, %fi2 [wrap]
    ret %s1
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. mul_ovf_u with dead overflow flag: 7 * 9 == 63
    %m = call @mul_dead_ovf(7, 9)
    %m_ok = cmp_eq.i64 %m, 63
    br %m_ok, .t2, .fail1

.t2:
    // 2. vector div_s/rem_s [wrap] on INT32_MIN / -1
    %va = splat.v4i32 -2147483648
    %vb = splat.v4i32 -1
    %vd = div_s.v4i32 %va, %vb [wrap]
    %vr = rem_s.v4i32 %va, %vb [wrap]
    %de = extract.v4i32 %vd, 2
    %re = extract.v4i32 %vr, 1
    %d_ok = cmp_eq.i32 %de, -2147483648
    %r_ok = cmp_eq.i32 %re, 0
    %dr_ok = and.i8 %d_ok, %r_ok
    br %dr_ok, .t3, .fail2

.t3:
    // 3. vector fptosi/fptoui [sat] sub-32b clamping
    %fp_hi = splat.v4f32 100000.0
    %fp_lo = splat.v4f32 -100000.0
    %si_hi = fptosi.v4i16 %fp_hi [sat]
    %si_lo = fptosi.v4i16 %fp_lo [sat]
    %ui_hi = fptoui.v4i16 %fp_hi [sat]
    %e_hi = extract.v4i16 %si_hi, 3
    %e_lo = extract.v4i16 %si_lo, 1
    %e_ui = extract.v4i16 %ui_hi, 2
    %ok1 = cmp_eq.i16 %e_hi, 32767
    %ok2 = cmp_eq.i16 %e_lo, -32768
    %ok3 = cmp_eq.i16 %e_ui, -1
    %ok12 = and.i8 %ok1, %ok2
    %ok_sat = and.i8 %ok12, %ok3
    br %ok_sat, .t4, .fail3

.t4:
    // 4. Cross-width 256-bit <-> 128-bit vector casts
    %bi = alloca.ptr 32, align 8
    %bi0 = ptr_add.i64 %bi, 0
    %bi1 = ptr_add.i64 %bi, 8
    %bi2 = ptr_add.i64 %bi, 16
    %bi3 = ptr_add.i64 %bi, 24
    store.i64 -5, %bi0, align 8
    store.i64 10, %bi1, align 8
    store.i64 20, %bi2, align 8
    store.i64 35, %bi3, align 8

    %bf = alloca.ptr 32, align 8
    %bf0 = ptr_add.i64 %bf, 0
    %bf1 = ptr_add.i64 %bf, 8
    %bf2 = ptr_add.i64 %bf, 16
    %bf3 = ptr_add.i64 %bf, 24
    store.f64 1.0, %bf0, align 8
    store.f64 2.0, %bf1, align 8
    store.f64 12.0, %bf2, align 8
    store.f64 4.0, %bf3, align 8

    // Expected: -5 + 35 + 12 = 42
    %xw = call @cross_width_casts(%bi, %bf)
    %xw_ok = cmp_eq.i64 %xw, 42
    br %xw_ok, .t5, .fail4

.t5:
    // 5. ARM64 lane-wise vector cast with live FP regs
    %v2_0 = splat.v2i32 -17
    %v2_1 = insert.v2i32 %v2_0, 29, 1
    %v2_ext = call @vec_cast_uninit(%v2_1, 1.5, 2.5, 3.5, 4.5)
    %x0 = extract.v2i64 %v2_ext, 0
    %x1 = extract.v2i64 %v2_ext, 1
    %xs = add.i64 %x0, %x1 [wrap]
    %x_ok = cmp_eq.i64 %xs, 12
    br %x_ok, .pass, .fail5

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
}
