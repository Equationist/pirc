target "aarch64-apple-darwin"

extern func @c_darwin_packed_stack(%g0: i64, %g1: i64, %g2: i64, %g3: i64, %g4: i64, %g5: i64, %g6: i64, %g7: i64, %s0: i8, %s1: i8, %s2: i16, %s3: i32, %s4: i8, %s5: i64) -> i64 cc "c" [nounwind]

export func @pir_many_args(%g0: i64, %g1: i64, %g2: i64, %g3: i64, %g4: i64, %g5: i64, %g6: i64, %g7: i64, %g8: i64, %g9: i64, %g10: i64, %g11: i64, %f0: f64, %f1: f64, %f2: f64, %f3: f64, %f4: f64, %f5: f64, %f6: f64, %f7: f64, %f8: f64, %f9: f64, %f10: f64, %f11: f64) -> i64 cc "c" [nounwind] {
.entry:
    %a0 = add.i64 %g0, %g1
    %a1 = add.i64 %g2, %g3
    %a2 = add.i64 %g4, %g5
    %a3 = add.i64 %g6, %g7
    %a4 = add.i64 %g8, %g9
    %a5 = add.i64 %g10, %g11
    %b0 = add.i64 %a0, %a1
    %b1 = add.i64 %a2, %a3
    %b2 = add.i64 %a4, %a5
    %c0 = add.i64 %b0, %b1
    %gsum = add.i64 %c0, %b2

    %d0 = fadd.f64 %f0, %f1
    %d1 = fadd.f64 %f2, %f3
    %d2 = fadd.f64 %f4, %f5
    %d3 = fadd.f64 %f6, %f7
    %d4 = fadd.f64 %f8, %f9
    %d5 = fadd.f64 %f10, %f11
    %e0 = fadd.f64 %d0, %d1
    %e1 = fadd.f64 %d2, %d3
    %e2 = fadd.f64 %d4, %d5
    %h0 = fadd.f64 %e0, %e1
    %fsum = fadd.f64 %h0, %e2
    %fi = fptosi.i64 %fsum [sat]

    %res = add.i64 %gsum, %fi
    ret %res
}

export func @pir_darwin_packed_stack(%g0: i64, %g1: i64, %g2: i64, %g3: i64, %g4: i64, %g5: i64, %g6: i64, %g7: i64, %s0: i8, %s1: i8, %s2: i16, %s3: i32, %s4: i8, %s5: i64) -> i64 cc "c" [nounwind] {
.entry:
    %a0 = add.i64 %g0, %g1
    %a1 = add.i64 %g2, %g3
    %a2 = add.i64 %g4, %g5
    %a3 = add.i64 %g6, %g7
    %b0 = add.i64 %a0, %a1
    %b1 = add.i64 %a2, %a3
    %gs = add.i64 %b0, %b1

    %e0 = sext.i64 %s0
    %e1 = zext.i64 %s1
    %e2 = sext.i64 %s2
    %e3 = sext.i64 %s3
    %e4 = sext.i64 %s4

    %t0 = add.i64 %e0, %e1
    %t1 = add.i64 %e2, %e3
    %t2 = add.i64 %e4, %s5
    %u0 = add.i64 %t0, %t1
    %u1 = add.i64 %u0, %t2
    %res = add.i64 %gs, %u1
    ret %res
}

export func @pir_drive_c_packed_stack() -> i32 cc "c" [nounwind] {
.entry:
    %v0 = const.i8 -5
    %v1 = const.i8 250
    %v2 = const.i16 -1000
    %v3 = const.i32 50000
    %v4 = const.i8 -3
    %got = call @c_darwin_packed_stack(1, 2, 3, 4, 5, 6, 7, 8, %v0, %v1, %v2, %v3, %v4, 1000000)
    %ok = cmp_eq.i64 %got, 1049278
    %res = zext.i32 %ok
    ret %res
}
