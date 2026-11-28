target "avr-unknown-elf"

// AVR soft-float (f32/f64), i32/i64/i128 wide integer legalization, and scalarized SIMD test.

func @add_i64(%a: i64, %b: i64) -> i64 cc "c" [nounwind] {
.entry:
    %r = add.i64 %a, %b
    ret %r
}

func @f64_poly(%x: f64) -> f64 cc "c" [nounwind] {
.entry:
    // x * x + 2.5 * x - 1.0
    %x2 = fmul.f64 %x, %x
    %t1 = fmul.f64 %x, 2.5
    %s1 = fadd.f64 %x2, %t1
    %r  = fsub.f64 %s1, 1.0
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Soft-float f32 arithmetic & conversions
    %i15 = const.i32 -15
    %i4  = const.i32 4
    %fa = sitofp.f32 %i15
    %fb = sitofp.f32 %i4
    %fd = fdiv.f32 %fa, %fb          // -3.75
    %fn = fneg.f32 %fd               // +3.75
    %fm = fmul.f32 %fn, 4.0          // 15.0
    %fi = fptosi.i32 %fm [sat]       // 15
    %ok1 = cmp_eq.i32 %fi, 15
    br %ok1, .t2, .fail(1)

.t2:
    // 2. Soft-float f64 & fpext/fptrunc & ordered/unordered NaN fcmp
    %r64 = call @f64_poly(3.0)       // 9 + 7.5 - 1 = 15.5
    %r32 = fptrunc.f32 %r64
    %re64 = fpext.f64 %r32
    %eq = fcmp_oeq.f64 %re64, 15.5
    %zero = const.f64 0.0
    %nan = fdiv.f64 %zero, %zero
    %u_ne = fcmp_une.f64 %nan, %nan  // 1
    %o_lt = fcmp_olt.f64 %nan, 1.0   // 0
    %not_olt = xor.i8 %o_lt, 1
    %ok2a = and.i8 %eq, %u_ne
    %ok2 = and.i8 %ok2a, %not_olt
    br %ok2, .t3, .fail(2)

.t3:
    // 3. 64-bit arithmetic, mul, div/rem, shifts across 4x i16 words
    %a64 = const.i64 100000000000
    %b64 = const.i64 23456789012
    %s64 = call @add_i64(%a64, %b64) // 123456789012
    %q64 = div_s.i64 %s64, 1000000   // 123456
    %r64i = rem_s.i64 %s64, 1000000  // 789012
    %ok3a = cmp_eq.i64 %q64, 123456
    %ok3b = cmp_eq.i64 %r64i, 789012
    %ok3 = and.i8 %ok3a, %ok3b
    br %ok3, .t4, .fail(3)

.t4:
    // 4. 128-bit arithmetic & division on an 8-bit CPU!
    %c1e9 = const.i64 1000000000
    %x128 = zext.i128 %c1e9
    %y128 = zext.i128 %c1e9
    %p128 = mul.i128 %x128, %y128    // 10^18
    %c25e7 = const.i64 250000000
    %d128 = zext.i128 %c25e7
    %q128 = div_u.i128 %p128, %d128  // 4000000000
    %q64_t = trunc.i64 %q128
    %ok4 = cmp_eq.i64 %q64_t, 4000000000
    br %ok4, .t5, .fail(4)

.t5:
    // 5. Scalarized SIMD v4i32 + v4f32 on AVR
    %v1 = splat.v4i32 10
    %v2 = splat.v4i32 7
    %vs = add.v4i32 %v1, %v2
    %e2 = extract.v4i32 %vs, 2
    %fv1 = splat.v4f32 2.5
    %fv2 = splat.v4f32 4.0
    %fvp = fmul.v4f32 %fv1, %fv2     // 10.0
    %fe0 = extract.v4f32 %fvp, 0
    %fi0 = fptosi.i32 %fe0 [sat]
    %ok5a = cmp_eq.i32 %e2, 17
    %ok5b = cmp_eq.i32 %fi0, 10
    %ok5 = and.i8 %ok5a, %ok5b
    br %ok5, .pass, .fail(5)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
