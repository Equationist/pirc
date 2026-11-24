// MODE: LIR
// EXPECT_RET: 63
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %b = const.i8 -5
    %sx = sext.i32 %b
    %zx = zext.i32 %b
    %tr = trunc.i8 %sx
    %c1 = cmp_eq.i32 %sx, -5
    %c2 = cmp_eq.i32 %zx, 251
    %c3 = cmp_eq.i8 %tr, -5
    %ok1 = and.i8 %c1, %c2
    %ok2 = and.i8 %ok1, %c3
    br %ok2, .floats, .fail

.floats:
    %fi = sitofp.f32 -42
    %fu = uitofp.f64 1000
    %fe = fpext.f64 %fi
    %ft = fptrunc.f32 %fu
    %si = fptosi.i32 %fe [sat]
    %ui = fptoui.i32 %ft [sat]
    // Saturating casts on NaN and huge values
    %nan_v = const.f64 nan
    %sat_nan = fptosi.i32 %nan_v [sat]
    %inf_v = const.f64 inf
    %sat_inf = fptosi.i32 %inf_v [sat]
    %ninf_v = const.f64 -inf
    %sat_ninf = fptoui.i32 %ninf_v [sat]
    %k1 = cmp_eq.i32 %si, -42
    %k2 = cmp_eq.i32 %ui, 1000
    %k3 = cmp_eq.i32 %sat_nan, 0
    %k4 = cmp_eq.i32 %sat_inf, 2147483647
    %k5 = cmp_eq.i32 %sat_ninf, 0
    %m1 = and.i8 %k1, %k2
    %m2 = and.i8 %k3, %k4
    %m3 = and.i8 %m1, %m2
    %m4 = and.i8 %m3, %k5
    br %m4, .bitcasts, .fail

.bitcasts:
    // IEEE 754 float 1.0f == 0x3F800000
    %one = const.f32 1.0
    %bits = bitcast.i32 %one
    %back = bitcast.f32 %bits
    %bc_ok = cmp_eq.i32 %bits, 0x3F800000
    %fp_ok = fcmp_oeq.f32 %back, 1.0
    // ptrtoint / inttoptr roundtrip on stack slot
    %slot = alloca.ptr 8, align 8
    store.i64 63, %slot, align 8
    %addr = ptrtoint.i64 %slot
    %p2 = inttoptr.ptr %addr
    %val64 = load.i64 %p2, align 8
    %val32 = trunc.i32 %val64
    %all_ok = and.i8 %bc_ok, %fp_ok
    br %all_ok, .done(%val32), .fail

.done(%res: i32):
    ret %res

.fail:
    trap
}
