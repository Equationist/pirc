// MODE: HIR
// EXPECT_RET: 74
target "aarch64-apple-darwin"

type @IntPair = { i32, i32 }
type @Vec2F   = { f32, f32 }
type @Big4    = { i64, i64, i64, i64 }

// 8B composite return (x0 on Darwin ARM64)
export func @make_pair(%out: retbuf(@IntPair), %a: i32, %b: i32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @IntPair, %out, 0
    %p1 = field_ptr @IntPair, %out, 1
    store.i32 %a, %p0, align 4
    store.i32 %b, %p1, align 4
    ret
}

// 2-element HFA return (s0, s1 on Darwin ARM64)
export func @make_vec2(%out: retbuf(@Vec2F), %x: f32, %y: f32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Vec2F, %out, 0
    %p1 = field_ptr @Vec2F, %out, 1
    store.f32 %x, %p0, align 4
    store.f32 %y, %p1, align 4
    ret
}

// 32B indirect return (x8 on Darwin ARM64); also tests aliasing when %out == %in in caller
export func @inc_big(%out: retbuf(@Big4), %in: byval(@Big4), %delta: i64) -> void cc "c" [nounwind] {
.entry:
    %i0 = field_ptr @Big4, %in, 0
    %i1 = field_ptr @Big4, %in, 1
    %i2 = field_ptr @Big4, %in, 2
    %i3 = field_ptr @Big4, %in, 3
    %v0 = load.i64 %i0, align 8
    %v1 = load.i64 %i1, align 8
    %v2 = load.i64 %i2, align 8
    %v3 = load.i64 %i3, align 8

    %n0 = add.i64 %v0, %delta
    %n1 = add.i64 %v1, %delta
    %n2 = add.i64 %v2, %delta
    %n3 = add.i64 %v3, %delta

    %o0 = field_ptr @Big4, %out, 0
    %o1 = field_ptr @Big4, %out, 1
    %o2 = field_ptr @Big4, %out, 2
    %o3 = field_ptr @Big4, %out, 3
    store.i64 %n0, %o0, align 8
    store.i64 %n1, %o1, align 8
    store.i64 %n2, %o2, align 8
    store.i64 %n3, %o3, align 8
    ret
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. IntPair: (11, 13) -> sum 24
    %ip = alloca @IntPair
    call @make_pair(%ip, 11, 13)
    %ip0 = field_ptr @IntPair, %ip, 0
    %ip1 = field_ptr @IntPair, %ip, 1
    %a = load.i32 %ip0, align 4
    %b = load.i32 %ip1, align 4
    %s_int = add.i32 %a, %b

    // 2. Vec2F: (4.0, 6.0) -> sum 10
    %vf = alloca @Vec2F
    call @make_vec2(%vf, 4.0, 6.0)
    %vf0 = field_ptr @Vec2F, %vf, 0
    %vf1 = field_ptr @Vec2F, %vf, 1
    %fx = load.f32 %vf0, align 4
    %fy = load.f32 %vf1, align 4
    %fsum = fadd.f32 %fx, %fy
    %s_flt = fptosi.i32 %fsum [sat]

    // 3. Big4 with in-place aliasing (%big passed as both retbuf and byval):
    // [1, 2, 3, 4] + 7 -> [8, 9, 10, 11] -> sum = 38; wait +2? 8+9+10+11 = 38; 24+10+38 = 72; add 2 -> 74
    %big = alloca @Big4
    %b0 = field_ptr @Big4, %big, 0
    %b1 = field_ptr @Big4, %big, 1
    %b2 = field_ptr @Big4, %big, 2
    %b3 = field_ptr @Big4, %big, 3
    store.i64 1, %b0, align 8
    store.i64 2, %b1, align 8
    store.i64 3, %b2, align 8
    store.i64 4, %b3, align 8
    call @inc_big(%big, %big, 7)

    %r0 = load.i64 %b0, align 8
    %r1 = load.i64 %b1, align 8
    %r2 = load.i64 %b2, align 8
    %r3 = load.i64 %b3, align 8
    %bs1 = add.i64 %r0, %r1
    %bs2 = add.i64 %r2, %r3
    %bs3 = add.i64 %bs1, %bs2
    %s_big = trunc.i32 %bs3

    // 24 + 10 + 38 + 2 = 74
    %t1 = add.i32 %s_int, %s_flt
    %t2 = add.i32 %t1, %s_big
    %res = add.i32 %t2, 2
    ret %res
}
