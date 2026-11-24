target "aarch64-apple-darwin"

extern func @c_darwin_odd_i128(%a: i32, %b: i128, %c: i64, %d: i128) -> i128 cc "c" [nounwind]
extern func @c_sum10_v4f32(%v0: v4f32, %v1: v4f32, %v2: v4f32, %v3: v4f32, %v4: v4f32, %v5: v4f32, %v6: v4f32, %v7: v4f32, %v8: v4f32, %v9: v4f32) -> v4f32 cc "c" [nounwind]

export func @pir_add_i128(%a: i128, %b: i128) -> i128 cc "c" [nounwind] {
.entry:
    %r = add.i128 %a, %b [wrap]
    ret %r
}

export func @pir_darwin_odd_i128(%a: i32, %b: i128, %c: i64, %d: i128) -> i128 cc "c" [nounwind] {
.entry:
    %a128 = sext.i128 %a
    %c128 = sext.i128 %c
    %s1 = add.i128 %a128, %b
    %s2 = add.i128 %c128, %d
    %res = add.i128 %s1, %s2
    ret %res
}

export func @pir_spill_i128(%a0: i64, %a1: i64, %a2: i64, %a3: i64, %a4: i64, %a5: i64, %a6: i64, %big: i128) -> i128 cc "c" [nounwind] {
.entry:
    %t0 = add.i64 %a0, %a1
    %t1 = add.i64 %a2, %a3
    %t2 = add.i64 %a4, %a5
    %t3 = add.i64 %t0, %t1
    %t4 = add.i64 %t2, %a6
    %sum64 = add.i64 %t3, %t4
    %sum128 = sext.i128 %sum64
    %res = add.i128 %sum128, %big
    ret %res
}

export func @pir_fma_v4f32(%a: v4f32, %b: v4f32, %c: v4f32) -> v4f32 cc "c" [nounwind] {
.entry:
    %p = fmul.v4f32 %a, %b
    %r = fadd.v4f32 %p, %c
    ret %r
}

export func @pir_add_v2f64(%a: v2f64, %b: v2f64) -> v2f64 cc "c" [nounwind] {
.entry:
    %r = fadd.v2f64 %a, %b
    ret %r
}

export func @pir_add_v4i32(%a: v4i32, %b: v4i32) -> v4i32 cc "c" [nounwind] {
.entry:
    %r = add.v4i32 %a, %b
    ret %r
}

export func @pir_add_v16i8(%a: v16i8, %b: v16i8) -> v16i8 cc "c" [nounwind] {
.entry:
    %r = add.v16i8 %a, %b
    ret %r
}

export func @pir_sum10_v4f32(%v0: v4f32, %v1: v4f32, %v2: v4f32, %v3: v4f32, %v4: v4f32, %v5: v4f32, %v6: v4f32, %v7: v4f32, %v8: v4f32, %v9: v4f32) -> v4f32 cc "c" [nounwind] {
.entry:
    %s0 = fadd.v4f32 %v0, %v1
    %s1 = fadd.v4f32 %v2, %v3
    %s2 = fadd.v4f32 %v4, %v5
    %s3 = fadd.v4f32 %v6, %v7
    %s4 = fadd.v4f32 %v8, %v9
    %t0 = fadd.v4f32 %s0, %s1
    %t1 = fadd.v4f32 %s2, %s3
    %t2 = fadd.v4f32 %t0, %t1
    %res = fadd.v4f32 %t2, %s4
    ret %res
}

export func @pir_drive_c_i128_simd() -> i32 cc "c" [nounwind] {
.entry:
    %one = const.i128 1
    %hi1 = shl.i128 %one, 64 [mask]
    %b = add.i128 %hi1, 5
    %d = add.i128 %hi1, 7
    %r = call @c_darwin_odd_i128(3, %b, 100, %d)
    %r_lo = trunc.i64 %r
    %r_hi128 = shr.i128 %r, 64 [mask]
    %r_hi = trunc.i64 %r_hi128
    %c1 = cmp_eq.i64 %r_lo, 115
    %c2 = cmp_eq.i64 %r_hi, 2

    %u1 = splat.v4f32 1.0
    %u2 = splat.v4f32 2.0
    %u9 = splat.v4f32 9.0
    %u10 = splat.v4f32 10.0
    %vs = call @c_sum10_v4f32(%u1, %u1, %u1, %u1, %u1, %u1, %u1, %u2, %u9, %u10)
    // 7*1 + 2 + 9 + 10 = 28.0
    %e0 = extract.v4f32 %vs, 0
    %c3 = fcmp_oeq.f32 %e0, 28.0

    %k1 = and.i8 %c1, %c2
    %ok = and.i8 %k1, %c3
    %res = zext.i32 %ok
    ret %res
}
