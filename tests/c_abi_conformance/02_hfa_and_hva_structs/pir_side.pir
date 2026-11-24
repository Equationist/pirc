target "aarch64-apple-darwin"

type @Hfa2F       = { f32, f32 }
type @Hfa3F       = { f32, f32, f32 }
type @Hfa4F       = { f32, f32, f32, f32 }
type @Hfa2D       = { f64, f64 }
type @Hfa4D       = { f64, f64, f64, f64 }
type @NestedHfa4F = { @Hfa2F, @Hfa2F }
type @NonHfa5F    = { [5 x f32] }
type @Hva2V4F     = { v4f32, v4f32 }
type @Hva4V4F     = { v4f32, v4f32, v4f32, v4f32 }
type @NonHva5V4F  = { [5 x v4f32] }

extern func @c_rt_hfa4f(%out: retbuf(@Hfa4F), %x: byval(@Hfa4F)) -> void cc "c" [nounwind]
extern func @c_rt_non_hfa5f(%out: retbuf(@NonHfa5F), %x: byval(@NonHfa5F)) -> void cc "c" [nounwind]
extern func @c_rt_hva2(%out: retbuf(@Hva2V4F), %x: byval(@Hva2V4F)) -> void cc "c" [nounwind]
extern func @c_spill_hfa4f(%f0: f32, %f1: f32, %f2: f32, %f3: f32, %f4: f32, %f5: f32, %h: byval(@Hfa4F)) -> f32 cc "c" [nounwind]

export func @pir_rt_hfa2f(%out: retbuf(@Hfa2F), %x: byval(@Hfa2F)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa2F, %x, 0
    %p1 = field_ptr @Hfa2F, %x, 1
    %v0 = load.f32 %p0, align 4
    %v1 = load.f32 %p1, align 4
    %r0 = fadd.f32 %v0, 1.0
    %r1 = fadd.f32 %v1, 2.0
    %o0 = field_ptr @Hfa2F, %out, 0
    %o1 = field_ptr @Hfa2F, %out, 1
    store.f32 %r0, %o0, align 4
    store.f32 %r1, %o1, align 4
    ret
}

export func @pir_rt_hfa3f(%out: retbuf(@Hfa3F), %x: byval(@Hfa3F)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa3F, %x, 0
    %p1 = field_ptr @Hfa3F, %x, 1
    %p2 = field_ptr @Hfa3F, %x, 2
    %v0 = load.f32 %p0, align 4
    %v1 = load.f32 %p1, align 4
    %v2 = load.f32 %p2, align 4
    %r0 = fadd.f32 %v0, 1.0
    %r1 = fadd.f32 %v1, 2.0
    %r2 = fadd.f32 %v2, 3.0
    %o0 = field_ptr @Hfa3F, %out, 0
    %o1 = field_ptr @Hfa3F, %out, 1
    %o2 = field_ptr @Hfa3F, %out, 2
    store.f32 %r0, %o0, align 4
    store.f32 %r1, %o1, align 4
    store.f32 %r2, %o2, align 4
    ret
}

export func @pir_rt_hfa4f(%out: retbuf(@Hfa4F), %x: byval(@Hfa4F)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4F, %x, 0
    %p1 = field_ptr @Hfa4F, %x, 1
    %p2 = field_ptr @Hfa4F, %x, 2
    %p3 = field_ptr @Hfa4F, %x, 3
    %v0 = load.f32 %p0, align 4
    %v1 = load.f32 %p1, align 4
    %v2 = load.f32 %p2, align 4
    %v3 = load.f32 %p3, align 4
    %r0 = fadd.f32 %v0, 1.0
    %r1 = fadd.f32 %v1, 2.0
    %r2 = fadd.f32 %v2, 3.0
    %r3 = fadd.f32 %v3, 4.0
    %o0 = field_ptr @Hfa4F, %out, 0
    %o1 = field_ptr @Hfa4F, %out, 1
    %o2 = field_ptr @Hfa4F, %out, 2
    %o3 = field_ptr @Hfa4F, %out, 3
    store.f32 %r0, %o0, align 4
    store.f32 %r1, %o1, align 4
    store.f32 %r2, %o2, align 4
    store.f32 %r3, %o3, align 4
    ret
}

export func @pir_rt_hfa2d(%out: retbuf(@Hfa2D), %x: byval(@Hfa2D)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa2D, %x, 0
    %p1 = field_ptr @Hfa2D, %x, 1
    %v0 = load.f64 %p0, align 8
    %v1 = load.f64 %p1, align 8
    %r0 = fadd.f64 %v0, 1.0
    %r1 = fadd.f64 %v1, 2.0
    %o0 = field_ptr @Hfa2D, %out, 0
    %o1 = field_ptr @Hfa2D, %out, 1
    store.f64 %r0, %o0, align 8
    store.f64 %r1, %o1, align 8
    ret
}

export func @pir_rt_hfa4d(%out: retbuf(@Hfa4D), %x: byval(@Hfa4D)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4D, %x, 0
    %p1 = field_ptr @Hfa4D, %x, 1
    %p2 = field_ptr @Hfa4D, %x, 2
    %p3 = field_ptr @Hfa4D, %x, 3
    %v0 = load.f64 %p0, align 8
    %v1 = load.f64 %p1, align 8
    %v2 = load.f64 %p2, align 8
    %v3 = load.f64 %p3, align 8
    %r0 = fadd.f64 %v0, 1.0
    %r3 = fadd.f64 %v3, 4.0
    %o0 = field_ptr @Hfa4D, %out, 0
    %o1 = field_ptr @Hfa4D, %out, 1
    %o2 = field_ptr @Hfa4D, %out, 2
    %o3 = field_ptr @Hfa4D, %out, 3
    store.f64 %r0, %o0, align 8
    store.f64 %v1, %o1, align 8
    store.f64 %v2, %o2, align 8
    store.f64 %r3, %o3, align 8
    ret
}

export func @pir_rt_nested_hfa4f(%out: retbuf(@NestedHfa4F), %x: byval(@NestedHfa4F)) -> void cc "c" [nounwind] {
.entry:
    memcpy %out, %x, 16, align 4
    %lo = field_ptr @NestedHfa4F, %out, 0
    %hi = field_ptr @NestedHfa4F, %out, 1
    %loa = field_ptr @Hfa2F, %lo, 0
    %hib = field_ptr @Hfa2F, %hi, 1
    %va = load.f32 %loa, align 4
    %vb = load.f32 %hib, align 4
    %ra = fadd.f32 %va, 1.0
    %rb = fadd.f32 %vb, 4.0
    store.f32 %ra, %loa, align 4
    store.f32 %rb, %hib, align 4
    ret
}

export func @pir_rt_non_hfa5f(%out: retbuf(@NonHfa5F), %x: byval(@NonHfa5F)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @NonHfa5F, %x, 0
    %oa = field_ptr @NonHfa5F, %out, 0
    memcpy %oa, %xa, 20, align 4
    %p0 = elem_ptr [5 x f32], %oa, 0
    %p4 = elem_ptr [5 x f32], %oa, 4
    %v0 = load.f32 %p0, align 4
    %v4 = load.f32 %p4, align 4
    %r0 = fadd.f32 %v0, 1.0
    %r4 = fadd.f32 %v4, 5.0
    store.f32 %r0, %p0, align 4
    store.f32 %r4, %p4, align 4
    ret
}

export func @pir_rt_hva2(%out: retbuf(@Hva2V4F), %x: byval(@Hva2V4F)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hva2V4F, %x, 0
    %p1 = field_ptr @Hva2V4F, %x, 1
    %v0 = load.v4f32 %p0, align 16
    %v1 = load.v4f32 %p1, align 16
    %one = splat.v4f32 1.0
    %two = splat.v4f32 2.0
    %r0 = fadd.v4f32 %v0, %one
    %r1 = fadd.v4f32 %v1, %two
    %o0 = field_ptr @Hva2V4F, %out, 0
    %o1 = field_ptr @Hva2V4F, %out, 1
    store.v4f32 %r0, %o0, align 16
    store.v4f32 %r1, %o1, align 16
    ret
}

export func @pir_rt_hva4(%out: retbuf(@Hva4V4F), %x: byval(@Hva4V4F)) -> void cc "c" [nounwind] {
.entry:
    memcpy %out, %x, 64, align 16
    %o0 = field_ptr @Hva4V4F, %out, 0
    %o3 = field_ptr @Hva4V4F, %out, 3
    %v0 = load.v4f32 %o0, align 16
    %v3 = load.v4f32 %o3, align 16
    %one = splat.v4f32 1.0
    %four = splat.v4f32 4.0
    %r0 = fadd.v4f32 %v0, %one
    %r3 = fadd.v4f32 %v3, %four
    store.v4f32 %r0, %o0, align 16
    store.v4f32 %r3, %o3, align 16
    ret
}

export func @pir_rt_non_hva5(%out: retbuf(@NonHva5V4F), %x: byval(@NonHva5V4F)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @NonHva5V4F, %x, 0
    %oa = field_ptr @NonHva5V4F, %out, 0
    memcpy %oa, %xa, 80, align 16
    %p0 = elem_ptr [5 x v4f32], %oa, 0
    %p4 = elem_ptr [5 x v4f32], %oa, 4
    %v0 = load.v4f32 %p0, align 16
    %v4 = load.v4f32 %p4, align 16
    %one = splat.v4f32 1.0
    %five = splat.v4f32 5.0
    %r0 = fadd.v4f32 %v0, %one
    %r4 = fadd.v4f32 %v4, %five
    store.v4f32 %r0, %p0, align 16
    store.v4f32 %r4, %p4, align 16
    ret
}

export func @pir_spill_hfa4f(%f0: f32, %f1: f32, %f2: f32, %f3: f32, %f4: f32, %f5: f32, %h: byval(@Hfa4F)) -> f32 cc "c" [nounwind] {
.entry:
    %s0 = fadd.f32 %f0, %f1
    %s1 = fadd.f32 %f2, %f3
    %s2 = fadd.f32 %f4, %f5
    %s3 = fadd.f32 %s0, %s1
    %sf = fadd.f32 %s3, %s2
    %h0 = field_ptr @Hfa4F, %h, 0
    %h1 = field_ptr @Hfa4F, %h, 1
    %h2 = field_ptr @Hfa4F, %h, 2
    %h3 = field_ptr @Hfa4F, %h, 3
    %a = load.f32 %h0, align 4
    %b = load.f32 %h1, align 4
    %c = load.f32 %h2, align 4
    %d = load.f32 %h3, align 4
    %hs0 = fadd.f32 %a, %b
    %hs1 = fadd.f32 %c, %d
    %hs = fadd.f32 %hs0, %hs1
    %res = fadd.f32 %sf, %hs
    ret %res
}

export func @pir_drive_c_hfa_hva() -> i32 cc "c" [nounwind] {
.entry:
    // Hfa4F -> C + spill
    %h4 = alloca @Hfa4F
    %h4o = alloca @Hfa4F
    %p0 = field_ptr @Hfa4F, %h4, 0
    %p1 = field_ptr @Hfa4F, %h4, 1
    %p2 = field_ptr @Hfa4F, %h4, 2
    %p3 = field_ptr @Hfa4F, %h4, 3
    store.f32 10.0, %p0, align 4
    store.f32 20.0, %p1, align 4
    store.f32 30.0, %p2, align 4
    store.f32 40.0, %p3, align 4
    call @c_rt_hfa4f(%h4o, %h4)
    %o3 = field_ptr @Hfa4F, %h4o, 3
    %v3 = load.f32 %o3, align 4
    %c1 = fcmp_oeq.f32 %v3, 44.0

    %sp = call @c_spill_hfa4f(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, %h4)
    %c2 = fcmp_oeq.f32 %sp, 121.0

    // NonHfa5F -> C
    %h5 = alloca @NonHfa5F
    %h5o = alloca @NonHfa5F
    memset %h5, 0, 20, align 4
    %a5 = field_ptr @NonHfa5F, %h5, 0
    %e5_4 = elem_ptr [5 x f32], %a5, 4
    store.f32 5.0, %e5_4, align 4
    call @c_rt_non_hfa5f(%h5o, %h5)
    %oa5 = field_ptr @NonHfa5F, %h5o, 0
    %oe5_4 = elem_ptr [5 x f32], %oa5, 4
    %v5_4 = load.f32 %oe5_4, align 4
    %c3 = fcmp_oeq.f32 %v5_4, 10.0

    // Hva2V4F -> C
    %hv = alloca @Hva2V4F, align 16
    %hvo = alloca @Hva2V4F, align 16
    %hv0 = field_ptr @Hva2V4F, %hv, 0
    %hv1 = field_ptr @Hva2V4F, %hv, 1
    %va = splat.v4f32 3.0
    %vb = splat.v4f32 7.0
    store.v4f32 %va, %hv0, align 16
    store.v4f32 %vb, %hv1, align 16
    call @c_rt_hva2(%hvo, %hv)
    %hvo1 = field_ptr @Hva2V4F, %hvo, 1
    %out_vb = load.v4f32 %hvo1, align 16
    %lane2 = extract.v4f32 %out_vb, 2
    %c4 = fcmp_oeq.f32 %lane2, 9.0

    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %c4
    %ok = and.i8 %k1, %k2
    %res = zext.i32 %ok
    ret %res
}
