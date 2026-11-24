// MODE: LIR
// EXPECT_RET: 68
target "aarch64-apple-darwin"

// High FP/NEON register pressure across a call:
// 14 live f64 values + 4 live v4f32 vectors across @clobber_fp force d8..d15 saves and FP/SIMD spills.
func @clobber_fp(%x: f64) -> f64 [nounwind] {
.entry:
    %a = fadd.f64 %x, 1.0
    %b = fmul.f64 %a, 2.0
    %c = fadd.f64 %b, 3.0
    %d = fsub.f64 %c, %b
    %e = fadd.f64 %d, %x
    %res = fsub.f64 %e, 1.0
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %f0  = const.f64 1.0
    %f1  = fadd.f64 %f0, 1.0
    %f2  = fadd.f64 %f1, 1.0
    %f3  = fadd.f64 %f2, 1.0
    %f4  = fadd.f64 %f3, 1.0
    %f5  = fadd.f64 %f4, 1.0
    %f6  = fadd.f64 %f5, 1.0
    %f7  = fadd.f64 %f6, 1.0
    %f8  = fadd.f64 %f7, 1.0
    %f9  = fadd.f64 %f8, 1.0
    %f10 = fadd.f64 %f9, 1.0
    %f11 = fadd.f64 %f10, 1.0
    %f12 = fadd.f64 %f11, 1.0
    %f13 = fadd.f64 %f12, 1.0

    %v0 = splat.v4f32 1.0
    %v1 = splat.v4f32 2.0
    %v2 = splat.v4f32 3.0
    %v3 = splat.v4f32 4.0

    // All 14 f64 + 4 v4f32 values remain live across this call:
    // clobber_fp(1.0) = 3.0
    %cf = call @clobber_fp(%f0)

    %s0 = fadd.f64 %f0, %f1
    %s1 = fadd.f64 %f2, %f3
    %s2 = fadd.f64 %f4, %f5
    %s3 = fadd.f64 %f6, %f7
    %s4 = fadd.f64 %f8, %f9
    %s5 = fadd.f64 %f10, %f11
    %s6 = fadd.f64 %f12, %f13
    %p0 = fadd.f64 %s0, %s1
    %p1 = fadd.f64 %s2, %s3
    %p2 = fadd.f64 %s4, %s5
    %q0 = fadd.f64 %p0, %p1
    %q1 = fadd.f64 %p2, %s6
    %ftot = fadd.f64 %q0, %q1
    %fwith_c = fadd.f64 %ftot, %cf

    %vs1 = fadd.v4f32 %v0, %v1
    %vs2 = fadd.v4f32 %v2, %v3
    %vsum = fadd.v4f32 %vs1, %vs2
    %lane0 = extract.v4f32 %vsum, 0
    %lane0_d = fpext.f64 %lane0

    // 1..14 = 105; + 3 = 108; + 10 = 118; - 50 = 68
    %all = fadd.f64 %fwith_c, %lane0_d
    %ai = fptosi.i32 %all [sat]
    %res = sub.i32 %ai, 50
    ret %res
}
