// MODE: LIR
// EXPECT_RET: 19
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = const.f64 3.5
    %b = const.f64 2.0
    %sum = fadd.f64 %a, %b
    %dif = fsub.f64 %sum, 1.5
    %prd = fmul.f64 %dif, 3.0 [fast]
    %quo = fdiv.f64 %prd, 2.0
    %neg = fneg.f64 %quo
    // sum=5.5, dif=4.0, prd=12.0, quo=6.0, neg=-6.0
    %c1 = fcmp_oeq.f64 %neg, -6.0
    %c2 = fcmp_olt.f64 %neg, %a
    %c3 = fcmp_oge.f64 %quo, 6.0
    // NaN behavior: ordered comparisons must be false, unordered must be true
    %nanv = const.f64 nan
    %o_eq = fcmp_oeq.f64 %nanv, %nanv
    %o_lt = fcmp_olt.f64 %nanv, 1.0
    %u_ne = fcmp_une.f64 %nanv, %nanv
    %u_lt = fcmp_ult.f64 %nanv, 1.0
    %u_gt = fcmp_ugt.f64 1.0, %nanv
    %not_oeq = xor.i8 %o_eq, 1
    %not_olt = xor.i8 %o_lt, 1
    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %not_oeq
    %k3 = and.i8 %not_olt, %u_ne
    %k4 = and.i8 %u_lt, %u_gt
    %m1 = and.i8 %k1, %k2
    %m2 = and.i8 %k3, %k4
    %ok1 = and.i8 %m1, %m2
    br %ok1, .inf_test, .fail

.inf_test:
    %pinf = const.f64 inf
    %ninf = const.f64 -inf
    %i1 = fcmp_ogt.f64 %pinf, %quo
    %i2 = fcmp_olt.f64 %ninf, %quo
    %ok2 = and.i8 %i1, %i2
    br %ok2, .done, .fail

.done:
    %qi = fptosi.i32 %quo [sat]
    %res = add.i32 %qi, 13
    ret %res

.fail:
    trap
}
