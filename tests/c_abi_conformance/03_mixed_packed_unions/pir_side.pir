target "aarch64-apple-darwin"

type @Mixed8     = { i32, f32 }
type @Mixed16    = { i64, f64 }
type @MixedPad24 = { i8, i64, i8, f32 }
type @Packed9    = packed { i8, i64 }
type @Pair32     = { i32, i32 }
type @Pair64     = { i64, i64 }
type @PairD      = { f64, f64 }
type @Union8     = union { i64, f64, @Pair32 }
type @Union16    = union { @Pair64, @PairD }
type @Union24    = union { [3 x i64], [3 x f64] }

extern func @c_rt_mixed16(%out: retbuf(@Mixed16), %x: byval(@Mixed16)) -> void cc "c" [nounwind]
extern func @c_rt_packed9(%out: retbuf(@Packed9), %x: byval(@Packed9)) -> void cc "c" [nounwind]
extern func @c_rt_union8_i2d(%out: retbuf(@Union8), %x: byval(@Union8)) -> void cc "c" [nounwind]

export func @pir_rt_mixed8(%out: retbuf(@Mixed8), %x: byval(@Mixed8)) -> void cc "c" [nounwind] {
.entry:
    %pi = field_ptr @Mixed8, %x, 0
    %pf = field_ptr @Mixed8, %x, 1
    %vi = load.i32 %pi, align 4
    %vf = load.f32 %pf, align 4
    %ri = add.i32 %vi, 1
    %rf = fadd.f32 %vf, 2.0
    %oi = field_ptr @Mixed8, %out, 0
    %of = field_ptr @Mixed8, %out, 1
    store.i32 %ri, %oi, align 4
    store.f32 %rf, %of, align 4
    ret
}

export func @pir_rt_mixed16(%out: retbuf(@Mixed16), %x: byval(@Mixed16)) -> void cc "c" [nounwind] {
.entry:
    %pi = field_ptr @Mixed16, %x, 0
    %pd = field_ptr @Mixed16, %x, 1
    %vi = load.i64 %pi, align 8
    %vd = load.f64 %pd, align 8
    %ri = add.i64 %vi, 10
    %rd = fadd.f64 %vd, 3.5
    %oi = field_ptr @Mixed16, %out, 0
    %od = field_ptr @Mixed16, %out, 1
    store.i64 %ri, %oi, align 8
    store.f64 %rd, %od, align 8
    ret
}

export func @pir_rt_mixed_pad(%out: retbuf(@MixedPad24), %x: byval(@MixedPad24)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @MixedPad24, %x, 0
    %p1 = field_ptr @MixedPad24, %x, 1
    %p2 = field_ptr @MixedPad24, %x, 2
    %p3 = field_ptr @MixedPad24, %x, 3
    %v0 = load.i8 %p0, align 1
    %v1 = load.i64 %p1, align 8
    %v2 = load.i8 %p2, align 1
    %v3 = load.f32 %p3, align 4
    %r0 = add.i8 %v0, 1
    %r1 = add.i64 %v1, 2
    %r2 = add.i8 %v2, 3
    %r3 = fadd.f32 %v3, 4.0
    %o0 = field_ptr @MixedPad24, %out, 0
    %o1 = field_ptr @MixedPad24, %out, 1
    %o2 = field_ptr @MixedPad24, %out, 2
    %o3 = field_ptr @MixedPad24, %out, 3
    store.i8 %r0, %o0, align 1
    store.i64 %r1, %o1, align 8
    store.i8 %r2, %o2, align 1
    store.f32 %r3, %o3, align 4
    ret
}

export func @pir_rt_packed9(%out: retbuf(@Packed9), %x: byval(@Packed9)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Packed9, %x, 0
    %p1 = field_ptr @Packed9, %x, 1
    %v0 = load.i8 %p0, align 1
    %v1 = load.i64 %p1, align 1
    %r0 = add.i8 %v0, 1
    %r1 = add.i64 %v1, 1
    %o0 = field_ptr @Packed9, %out, 0
    %o1 = field_ptr @Packed9, %out, 1
    store.i8 %r0, %o0, align 1
    store.i64 %r1, %o1, align 1
    ret
}

export func @pir_rt_union8_d2i(%out: retbuf(@Union8), %x: byval(@Union8)) -> void cc "c" [nounwind] {
.entry:
    %pp = field_ptr @Union8, %x, 2
    %phi = field_ptr @Pair32, %pp, 1
    %hi32 = load.i32 %phi, align 4
    %hi64 = zext.i64 %hi32
    %oi = field_ptr @Union8, %out, 0
    store.i64 %hi64, %oi, align 8
    ret
}

export func @pir_rt_union16(%out: retbuf(@Union16), %x: byval(@Union16)) -> void cc "c" [nounwind] {
.entry:
    %xp = field_ptr @Union16, %x, 0
    %p0 = field_ptr @Pair64, %xp, 0
    %p1 = field_ptr @Pair64, %xp, 1
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %r0 = add.i64 %v0, 5
    %r1 = add.i64 %v1, 7
    %op = field_ptr @Union16, %out, 0
    %o0 = field_ptr @Pair64, %op, 0
    %o1 = field_ptr @Pair64, %op, 1
    store.i64 %r0, %o0, align 8
    store.i64 %r1, %o1, align 8
    ret
}

export func @pir_rt_union24(%out: retbuf(@Union24), %x: byval(@Union24)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @Union24, %x, 0
    %oa = field_ptr @Union24, %out, 0
    memcpy %oa, %xa, 24, align 8
    %p0 = elem_ptr [3 x i64], %oa, 0
    %p2 = elem_ptr [3 x i64], %oa, 2
    %v0 = load.i64 %p0, align 8
    %v2 = load.i64 %p2, align 8
    %r0 = add.i64 %v0, 1
    %r2 = add.i64 %v2, 3
    store.i64 %r0, %p0, align 8
    store.i64 %r2, %p2, align 8
    ret
}

export func @pir_drive_c_mixed() -> i32 cc "c" [nounwind] {
.entry:
    %m = alloca @Mixed16
    %mo = alloca @Mixed16
    %mi = field_ptr @Mixed16, %m, 0
    %md = field_ptr @Mixed16, %m, 1
    store.i64 50, %mi, align 8
    store.f64 1.5, %md, align 8
    call @c_rt_mixed16(%mo, %m)
    %moi = field_ptr @Mixed16, %mo, 0
    %mod = field_ptr @Mixed16, %mo, 1
    %ri = load.i64 %moi, align 8
    %rd = load.f64 %mod, align 8
    %c1 = cmp_eq.i64 %ri, 150
    %c2 = fcmp_oeq.f64 %rd, 4.0

    %p = alloca @Packed9
    %po = alloca @Packed9
    %pt = field_ptr @Packed9, %p, 0
    %pv = field_ptr @Packed9, %p, 1
    store.i8 3, %pt, align 1
    store.i64 200, %pv, align 1
    call @c_rt_packed9(%po, %p)
    %pot = field_ptr @Packed9, %po, 0
    %pov = field_ptr @Packed9, %po, 1
    %rt = load.i8 %pot, align 1
    %rv = load.i64 %pov, align 1
    %c3 = cmp_eq.i8 %rt, 4
    %c4 = cmp_eq.i64 %rv, 1200

    %u = alloca @Union8
    %uo = alloca @Union8
    %ui = field_ptr @Union8, %u, 0
    store.i64 12, %ui, align 8
    call @c_rt_union8_i2d(%uo, %u)
    %uod = field_ptr @Union8, %uo, 1
    %ud = load.f64 %uod, align 8
    %c5 = fcmp_oeq.f64 %ud, 12.5

    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %c4
    %k3 = and.i8 %k1, %k2
    %ok = and.i8 %k3, %c5
    %res = zext.i32 %ok
    ret %res
}
