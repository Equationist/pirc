// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_LIR_CONTAINS: fadd.f64
// EXPECT_LIR_CONTAINS: fmul.f64
// EXPECT_LIR_FORBIDS: ret 0
// EXPECT_LIR_FORBIDS: ret -2147483648
target "aarch64-apple-darwin"

// Semantic honesty:
// 1. Trapping div-by-zero / signed overflow must NEVER fold to a non-trapping constant.
export func @trap_div0() -> i32 cc "c" [nounwind] {
.entry:
    %r = div_s.i32 10, 0 [trap]
    ret %r
}

export func @trap_ovf() -> i32 cc "c" [nounwind] {
.entry:
    %r = add.i32 2147483647, 1 [trap_s]
    ret %r
}

// 2. Strict IEEE-754 float ops without [fast] must NOT simplify x + 0.0 -> x or x * 0.0 -> 0.0
//    (-0.0 + 0.0 = +0.0; Inf * 0.0 = NaN; NaN propagation).
export func @strict_ieee(%x: f64) -> f64 cc "c" [nounwind] {
.entry:
    %a = fadd.f64 %x, 0.0
    %b = fmul.f64 %a, 0.0
    ret %b
}
