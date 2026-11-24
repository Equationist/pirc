target "x86_64-unknown-linux-gnu"

export func @arith_mix(%a: i32, %b: i32, %x: f64, %y: f64) -> i32 {
.entry:
    %s1 = add.i32 %a, %b [wrap]
    %s2 = sub.i32 %s1, 1 [trap_s]
    %s3 = mul.i32 %s2, 3 [unsafe_s, unsafe_u]
    %q  = div_s.i32 %s3, %b [trap]
    %r  = rem_u.i32 %q, 7 [unsafe]
    %sh = shl.i32 %r, 2 [mask]
    %ar = sar.i32 %sh, 1 [trap]
    %fp = fadd.f64 %x, %y [fast]
    %ng = fneg.f64 %fp
    %ok = fcmp_olt.f64 %ng, 10.5
    %uo = fcmp_une.f64 %x, nan
    %ci = fptosi.i32 %ng [sat]
    %or = or.i8 %ok, %uo
    %r2 = select.i32 %or, %ci, %ar
    ret %r2
}
