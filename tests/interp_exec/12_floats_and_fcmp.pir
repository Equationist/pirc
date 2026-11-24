// EXPECT_OUT: -6.50 0 1 1 0
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%.2f %u %u %u %u\n", 0 }

export func @main() -> i32 {
.entry:
    %a  = const.f64 3.25
    %b  = fmul.f64 %a, 2.0
    %nb = fneg.f64 %b
    %c0 = fcmp_oeq.f64 nan, nan
    %c1 = fcmp_ueq.f64 nan, nan
    %c2 = fcmp_olt.f64 %nb, inf
    %c3 = fcmp_ogt.f64 -inf, %nb
    %i0 = zext.i32 %c0
    %i1 = zext.i32 %c1
    %i2 = zext.i32 %c2
    %i3 = zext.i32 %c3
    %f  = addr @fmt
    call @printf(%f, %nb, %i0, %i1, %i2, %i3)
    ret 0
}
