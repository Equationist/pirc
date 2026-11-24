target "aarch64-apple-darwin"

type @S1  = { i8 }
type @S2  = { i16 }
type @S3  = { i8, i8, i8 }
type @S4  = { i32 }
type @S5  = { [5 x i8] }
type @S7  = { [7 x i8] }
type @S8  = { i32, i32 }
type @S12 = { i32, i32, i32 }
type @S16 = { i64, i64 }
type @S17 = { i64, i64, i8 }
type @S24 = { i64, i64, i64 }
type @S32 = { [4 x i64] }
type @S64 = { [8 x i64] }

extern func @c_rt_s3(%out: retbuf(@S3), %x: byval(@S3)) -> void cc "c" [nounwind]
extern func @c_rt_s16(%out: retbuf(@S16), %x: byval(@S16)) -> void cc "c" [nounwind]
extern func @c_rt_s17(%out: retbuf(@S17), %x: byval(@S17)) -> void cc "c" [nounwind]
extern func @c_rt_s64(%out: retbuf(@S64), %x: byval(@S64)) -> void cc "c" [nounwind]
extern func @c_spill_s16(%a0: i64, %a1: i64, %a2: i64, %a3: i64, %a4: i64, %a5: i64, %a6: i64, %s: byval(@S16)) -> i64 cc "c" [nounwind]

export func @pir_rt_s1(%out: retbuf(@S1), %x: byval(@S1)) -> void cc "c" [nounwind] {
.entry:
    %p = field_ptr @S1, %x, 0
    %v = load.i8 %p, align 1
    %r = add.i8 %v, 1
    %o = field_ptr @S1, %out, 0
    store.i8 %r, %o, align 1
    ret
}

export func @pir_rt_s2(%out: retbuf(@S2), %x: byval(@S2)) -> void cc "c" [nounwind] {
.entry:
    %p = field_ptr @S2, %x, 0
    %v = load.i16 %p, align 2
    %r = add.i16 %v, 5
    %o = field_ptr @S2, %out, 0
    store.i16 %r, %o, align 2
    ret
}

export func @pir_rt_s3(%out: retbuf(@S3), %x: byval(@S3)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S3, %x, 0
    %p1 = field_ptr @S3, %x, 1
    %p2 = field_ptr @S3, %x, 2
    %v0 = load.i8 %p0, align 1
    %v1 = load.i8 %p1, align 1
    %v2 = load.i8 %p2, align 1
    store.i8 0xAA, %p0, align 1
    %r0 = add.i8 %v0, 1
    %r1 = add.i8 %v1, 2
    %r2 = add.i8 %v2, 3
    %o0 = field_ptr @S3, %out, 0
    %o1 = field_ptr @S3, %out, 1
    %o2 = field_ptr @S3, %out, 2
    store.i8 %r0, %o0, align 1
    store.i8 %r1, %o1, align 1
    store.i8 %r2, %o2, align 1
    ret
}

export func @pir_rt_s4(%out: retbuf(@S4), %x: byval(@S4)) -> void cc "c" [nounwind] {
.entry:
    %p = field_ptr @S4, %x, 0
    %v = load.i32 %p, align 4
    %r = add.i32 %v, 10
    %o = field_ptr @S4, %out, 0
    store.i32 %r, %o, align 4
    ret
}

export func @pir_rt_s5(%out: retbuf(@S5), %x: byval(@S5)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @S5, %x, 0
    %oa = field_ptr @S5, %out, 0
    memcpy %oa, %xa, 5, align 1
    %p0 = elem_ptr [5 x i8], %oa, 0
    %p4 = elem_ptr [5 x i8], %oa, 4
    %v0 = load.i8 %p0, align 1
    %v4 = load.i8 %p4, align 1
    %r0 = add.i8 %v0, 1
    %r4 = add.i8 %v4, 5
    store.i8 %r0, %p0, align 1
    store.i8 %r4, %p4, align 1
    ret
}

export func @pir_rt_s7(%out: retbuf(@S7), %x: byval(@S7)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @S7, %x, 0
    %oa = field_ptr @S7, %out, 0
    memcpy %oa, %xa, 7, align 1
    %p0 = elem_ptr [7 x i8], %oa, 0
    %p6 = elem_ptr [7 x i8], %oa, 6
    %v0 = load.i8 %p0, align 1
    %v6 = load.i8 %p6, align 1
    %r0 = add.i8 %v0, 1
    %r6 = add.i8 %v6, 7
    store.i8 %r0, %p0, align 1
    store.i8 %r6, %p6, align 1
    ret
}

export func @pir_rt_s8(%out: retbuf(@S8), %x: byval(@S8)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S8, %x, 0
    %p1 = field_ptr @S8, %x, 1
    %v0 = load.i32 %p0, align 4
    %v1 = load.i32 %p1, align 4
    %r0 = add.i32 %v0, 1
    %r1 = add.i32 %v1, 2
    %o0 = field_ptr @S8, %out, 0
    %o1 = field_ptr @S8, %out, 1
    store.i32 %r0, %o0, align 4
    store.i32 %r1, %o1, align 4
    ret
}

export func @pir_rt_s12(%out: retbuf(@S12), %x: byval(@S12)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S12, %x, 0
    %p1 = field_ptr @S12, %x, 1
    %p2 = field_ptr @S12, %x, 2
    %v0 = load.i32 %p0, align 4
    %v1 = load.i32 %p1, align 4
    %v2 = load.i32 %p2, align 4
    %r0 = add.i32 %v0, 1
    %r1 = add.i32 %v1, 2
    %r2 = add.i32 %v2, 3
    %o0 = field_ptr @S12, %out, 0
    %o1 = field_ptr @S12, %out, 1
    %o2 = field_ptr @S12, %out, 2
    store.i32 %r0, %o0, align 4
    store.i32 %r1, %o1, align 4
    store.i32 %r2, %o2, align 4
    ret
}

export func @pir_rt_s16(%out: retbuf(@S16), %x: byval(@S16)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S16, %x, 0
    %p1 = field_ptr @S16, %x, 1
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    store.i64 -1, %p0, align 8
    %r0 = add.i64 %v0, 10
    %r1 = add.i64 %v1, 20
    %o0 = field_ptr @S16, %out, 0
    %o1 = field_ptr @S16, %out, 1
    store.i64 %r0, %o0, align 8
    store.i64 %r1, %o1, align 8
    ret
}

export func @pir_rt_s17(%out: retbuf(@S17), %x: byval(@S17)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S17, %x, 0
    %p1 = field_ptr @S17, %x, 1
    %p2 = field_ptr @S17, %x, 2
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %v2 = load.i8 %p2, align 1
    store.i64 -2, %p0, align 8
    %r0 = add.i64 %v0, 1
    %r1 = add.i64 %v1, 2
    %r2 = add.i8 %v2, 3
    %o0 = field_ptr @S17, %out, 0
    %o1 = field_ptr @S17, %out, 1
    %o2 = field_ptr @S17, %out, 2
    store.i64 %r0, %o0, align 8
    store.i64 %r1, %o1, align 8
    store.i8 %r2, %o2, align 1
    ret
}

export func @pir_rt_s24(%out: retbuf(@S24), %x: byval(@S24)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S24, %x, 0
    %p1 = field_ptr @S24, %x, 1
    %p2 = field_ptr @S24, %x, 2
    %v0 = load.i64 %p0, align 8
    %v1 = load.i64 %p1, align 8
    %v2 = load.i64 %p2, align 8
    %r0 = add.i64 %v0, 1
    %r1 = add.i64 %v1, 2
    %r2 = add.i64 %v2, 3
    %o0 = field_ptr @S24, %out, 0
    %o1 = field_ptr @S24, %out, 1
    %o2 = field_ptr @S24, %out, 2
    store.i64 %r0, %o0, align 8
    store.i64 %r1, %o1, align 8
    store.i64 %r2, %o2, align 8
    ret
}

export func @pir_rt_s32(%out: retbuf(@S32), %x: byval(@S32)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @S32, %x, 0
    %oa = field_ptr @S32, %out, 0
    memcpy %oa, %xa, 32, align 8
    %p0 = elem_ptr [4 x i64], %oa, 0
    %p3 = elem_ptr [4 x i64], %oa, 3
    %v0 = load.i64 %p0, align 8
    %v3 = load.i64 %p3, align 8
    %r0 = add.i64 %v0, 1
    %r3 = add.i64 %v3, 4
    store.i64 %r0, %p0, align 8
    store.i64 %r3, %p3, align 8
    ret
}

export func @pir_rt_s64(%out: retbuf(@S64), %x: byval(@S64)) -> void cc "c" [nounwind] {
.entry:
    %xa = field_ptr @S64, %x, 0
    %oa = field_ptr @S64, %out, 0
    memcpy %oa, %xa, 64, align 8
    %p0 = elem_ptr [8 x i64], %oa, 0
    %p7 = elem_ptr [8 x i64], %oa, 7
    %v0 = load.i64 %p0, align 8
    %v7 = load.i64 %p7, align 8
    %x0 = elem_ptr [8 x i64], %xa, 0
    store.i64 -3, %x0, align 8
    %r0 = add.i64 %v0, 1
    %r7 = add.i64 %v7, 8
    store.i64 %r0, %p0, align 8
    store.i64 %r7, %p7, align 8
    ret
}

export func @pir_spill_s16(%a0: i64, %a1: i64, %a2: i64, %a3: i64, %a4: i64, %a5: i64, %a6: i64, %s: byval(@S16)) -> i64 cc "c" [nounwind] {
.entry:
    %t0 = add.i64 %a0, %a1
    %t1 = add.i64 %a2, %a3
    %t2 = add.i64 %a4, %a5
    %t3 = add.i64 %t0, %t1
    %t4 = add.i64 %t2, %a6
    %sum = add.i64 %t3, %t4
    %sp0 = field_ptr @S16, %s, 0
    %sp1 = field_ptr @S16, %s, 1
    %sa = load.i64 %sp0, align 8
    %sb = load.i64 %sp1, align 8
    %sa3 = mul.i64 %sa, 3
    %sb5 = mul.i64 %sb, 5
    %u1 = add.i64 %sum, %sa3
    %res = add.i64 %u1, %sb5
    ret %res
}

export func @pir_drive_c_calls() -> i64 cc "c" [nounwind] {
.entry:
    // 1. S3 -> C
    %in3 = alloca @S3
    %out3 = alloca @S3
    %i3_0 = field_ptr @S3, %in3, 0
    %i3_1 = field_ptr @S3, %in3, 1
    %i3_2 = field_ptr @S3, %in3, 2
    store.i8 10, %i3_0, align 1
    store.i8 20, %i3_1, align 1
    store.i8 30, %i3_2, align 1
    call @c_rt_s3(%out3, %in3)
    %o3_0 = field_ptr @S3, %out3, 0
    %o3_2 = field_ptr @S3, %out3, 2
    %v3_0 = load.i8 %o3_0, align 1
    %v3_2 = load.i8 %o3_2, align 1
    %orig3_0 = load.i8 %i3_0, align 1
    %c1 = cmp_eq.i8 %v3_0, 11
    %c2 = cmp_eq.i8 %v3_2, 33
    %c3 = cmp_eq.i8 %orig3_0, 10

    // 2. S16 -> C + spill
    %in16 = alloca @S16
    %out16 = alloca @S16
    %i16_0 = field_ptr @S16, %in16, 0
    %i16_1 = field_ptr @S16, %in16, 1
    store.i64 100, %i16_0, align 8
    store.i64 200, %i16_1, align 8
    call @c_rt_s16(%out16, %in16)
    %o16_0 = field_ptr @S16, %out16, 0
    %o16_1 = field_ptr @S16, %out16, 1
    %v16_0 = load.i64 %o16_0, align 8
    %v16_1 = load.i64 %o16_1, align 8
    %orig16_0 = load.i64 %i16_0, align 8
    %sp = call @c_spill_s16(1, 2, 3, 4, 5, 6, 7, %in16)
    %c4 = cmp_eq.i64 %v16_0, 110
    %c5 = cmp_eq.i64 %v16_1, 220
    %c6 = cmp_eq.i64 %orig16_0, 100
    %c7 = cmp_eq.i64 %sp, 1328

    // 3. S17 & S64 -> C
    %in17 = alloca @S17
    %out17 = alloca @S17
    memset %in17, 0, 24, align 8
    %i17_0 = field_ptr @S17, %in17, 0
    %i17_1 = field_ptr @S17, %in17, 1
    %i17_2 = field_ptr @S17, %in17, 2
    store.i64 5, %i17_0, align 8
    store.i64 6, %i17_1, align 8
    store.i8 7, %i17_2, align 1
    call @c_rt_s17(%out17, %in17)
    %o17_0 = field_ptr @S17, %out17, 0
    %o17_2 = field_ptr @S17, %out17, 2
    %v17_0 = load.i64 %o17_0, align 8
    %v17_2 = load.i8 %o17_2, align 1
    %orig17_0 = load.i64 %i17_0, align 8
    %c8 = cmp_eq.i64 %v17_0, 6
    %c9 = cmp_eq.i8 %v17_2, 10
    %c10 = cmp_eq.i64 %orig17_0, 5

    %in64 = alloca @S64
    %out64 = alloca @S64
    memset %in64, 0, 64, align 8
    %a64 = field_ptr @S64, %in64, 0
    %e64_0 = elem_ptr [8 x i64], %a64, 0
    %e64_7 = elem_ptr [8 x i64], %a64, 7
    store.i64 10, %e64_0, align 8
    store.i64 80, %e64_7, align 8
    call @c_rt_s64(%out64, %in64)
    %oa64 = field_ptr @S64, %out64, 0
    %oe64_0 = elem_ptr [8 x i64], %oa64, 0
    %oe64_7 = elem_ptr [8 x i64], %oa64, 7
    %v64_0 = load.i64 %oe64_0, align 8
    %v64_7 = load.i64 %oe64_7, align 8
    %orig64_0 = load.i64 %e64_0, align 8
    %c11 = cmp_eq.i64 %v64_0, 11
    %c12 = cmp_eq.i64 %v64_7, 88
    %c13 = cmp_eq.i64 %orig64_0, 10

    %k1 = and.i8 %c1, %c2
    %k2 = and.i8 %c3, %c4
    %k3 = and.i8 %c5, %c6
    %k4 = and.i8 %c7, %c8
    %k5 = and.i8 %c9, %c10
    %k6 = and.i8 %c11, %c12
    %m1 = and.i8 %k1, %k2
    %m2 = and.i8 %k3, %k4
    %m3 = and.i8 %k5, %k6
    %n1 = and.i8 %m1, %m2
    %n2 = and.i8 %m3, %c13
    %ok = and.i8 %n1, %n2
    %res = zext.i64 %ok
    ret %res
}
