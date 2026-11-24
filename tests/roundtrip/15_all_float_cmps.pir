target "x86_64-unknown-linux-gnu"

export func @float_ops(%a: f64, %b: f64) -> i8 {
.entry:
    %s  = fadd.f64 %a, 0x1.8p+1
    %d  = fsub.f64 %s, 1e-5 [fast]
    %m  = fmul.f64 %d, %b
    %q  = fdiv.f64 %m, 2.5 [fast]
    %n  = fneg.f64 %q
    %c0 = fcmp_oeq.f64 %n, inf
    %c1 = fcmp_one.f64 %n, -inf
    %c2 = fcmp_olt.f64 %n, %b
    %c3 = fcmp_ole.f64 %n, %b
    %c4 = fcmp_ogt.f64 %n, %b
    %c5 = fcmp_oge.f64 %n, %b
    %c6 = fcmp_ueq.f64 %n, nan
    %c7 = fcmp_une.f64 %n, nan
    %c8 = fcmp_ult.f64 %n, %b
    %c9 = fcmp_ule.f64 %n, %b
    %ca = fcmp_ugt.f64 %n, %b
    %cb = fcmp_uge.f64 %n, %b
    %r0 = or.i8 %c0, %c1
    %r1 = or.i8 %c2, %c3
    %r2 = or.i8 %c4, %c5
    %r3 = or.i8 %c6, %c7
    %r4 = or.i8 %c8, %c9
    %r5 = or.i8 %ca, %cb
    %t0 = xor.i8 %r0, %r1
    %t1 = xor.i8 %r2, %r3
    %t2 = xor.i8 %r4, %r5
    %u0 = or.i8 %t0, %t1
    %u1 = or.i8 %u0, %t2
    ret %u1
}
