// MODE: LIR
// EXPECT_RET: 71
target "aarch64-apple-darwin"

// Stack-passed arguments (>8 GP, >8 FP, and Darwin naturally packed sub-word stack args):
export func @sum12_gp(%a0: i64, %a1: i64, %a2: i64, %a3: i64, %a4: i64, %a5: i64, %a6: i64, %a7: i64, %a8: i64, %a9: i64, %a10: i64, %a11: i64) -> i64 cc "c" [nounwind] {
.entry:
    %r0 = add.i64 %a0, %a1
    %r1 = add.i64 %a2, %a3
    %r2 = add.i64 %a4, %a5
    %r3 = add.i64 %a6, %a7
    %reg_s = add.i64 %r0, %r1
    %reg_s2 = add.i64 %r2, %r3
    %reg_tot = add.i64 %reg_s, %reg_s2

    // Weighted sum of stack args a8..a11 catches slot-offset transposition bugs
    %w9  = mul.i64 %a9, 2
    %w10 = mul.i64 %a10, 3
    %w11 = mul.i64 %a11, 4
    %st0 = add.i64 %a8, %w9
    %st1 = add.i64 %w10, %w11
    %st_tot = add.i64 %st0, %st1
    %res = add.i64 %reg_tot, %st_tot
    ret %res
}

export func @sum12_fp(%f0: f64, %f1: f64, %f2: f64, %f3: f64, %f4: f64, %f5: f64, %f6: f64, %f7: f64, %f8: f64, %f9: f64, %f10: f64, %f11: f64) -> f64 cc "c" [nounwind] {
.entry:
    %r0 = fadd.f64 %f0, %f1
    %r1 = fadd.f64 %f2, %f3
    %r2 = fadd.f64 %f4, %f5
    %r3 = fadd.f64 %f6, %f7
    %ra = fadd.f64 %r0, %r1
    %rb = fadd.f64 %r2, %r3
    %reg_tot = fadd.f64 %ra, %rb

    %w9  = fmul.f64 %f9, 2.0
    %w10 = fmul.f64 %f10, 3.0
    %w11 = fmul.f64 %f11, 4.0
    %st0 = fadd.f64 %f8, %w9
    %st1 = fadd.f64 %w10, %w11
    %st_tot = fadd.f64 %st0, %st1
    %res = fadd.f64 %reg_tot, %st_tot
    ret %res
}

// Darwin ARM64 packs non-variadic sub-word stack args (%s0..%s4) to natural alignment!
export func @packed_stack_args(%r0: i64, %r1: i64, %r2: i64, %r3: i64, %r4: i64, %r5: i64, %r6: i64, %r7: i64, %s0: i8, %s1: i16, %s2: i32, %s3: i64, %s4: i8) -> i64 cc "c" [nounwind] {
.entry:
    %z0 = zext.i64 %s0
    %z1 = zext.i64 %s1
    %z2 = zext.i64 %s2
    %z4 = zext.i64 %s4
    %t0 = add.i64 %z0, %z1
    %t1 = add.i64 %z2, %s3
    %t2 = add.i64 %t0, %t1
    %t3 = add.i64 %t2, %z4
    %t4 = add.i64 %t3, %r0
    %res = add.i64 %t4, %r7
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 8 + (2*1 + 3*2 + 4*3 + 5*4) = 8 + 40 = 48
    %g = call @sum12_gp(1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 5)
    // 4.0 + (1*1 + 2*2 + 3*3 + 4*4) = 4.0 + 30.0 = 34.0
    %fd = call @sum12_fp(0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 1.0, 2.0, 3.0, 4.0)
    %fi = fptosi.i64 %fd [sat]
    // 3 + 5 + 7 + 11 + 13 = 39
    %p = call @packed_stack_args(0, 0, 0, 0, 0, 0, 0, 0, 3, 5, 7, 11, 13)

    // 48 + 34 + 39 - 50 = 71
    %s1 = add.i64 %g, %fi
    %s2 = add.i64 %s1, %p
    %s3 = sub.i64 %s2, 50
    %res = trunc.i32 %s3
    ret %res
}
