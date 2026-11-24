// MODE: LIR
// REQUIRES_SIMD: 128
// EXPECT_RET: 41
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %buf = alloca.ptr 16, align 16
    %p0 = ptr_add.i64 %buf, 0 [inbounds]
    %p1 = ptr_add.i64 %buf, 4 [inbounds]
    %p2 = ptr_add.i64 %buf, 8 [inbounds]
    %p3 = ptr_add.i64 %buf, 12 [inbounds]
    store.f32 2.0, %p0, align 4
    store.f32 5.0, %p1, align 4
    store.f32 8.0, %p2, align 4
    store.f32 12.0, %p3, align 4

    %v = load.v4f32 %buf, align 16
    %gain = splat.v4f32 2.0
    %cap = splat.v4f32 15.0
    // scaled = [4.0, 10.0, 16.0, 24.0]
    %scaled = fmul.v4f32 %v, %gain [fast]
    // clamped = [4.0, 10.0, 15.0, 15.0]
    %over = fcmp_ogt.v4f32 %scaled, %cap
    %clamped = select.v4f32 %over, %cap, %scaled
    // shuffle [0, 1, 2, 4] with gain=[2,2,2,2] -> [4.0, 10.0, 15.0, 2.0]
    %shuf = shuffle.v4f32 %clamped, %gain [0, 1, 2, 4]
    // insert lane 3 = 12.0 -> [4.0, 10.0, 15.0, 12.0] -> sum = 41.0
    %upd = insert.v4f32 %shuf, 12.0, 3
    store.v4f32 %upd, %buf, align 16
    %reloaded = load.v4f32 %buf, align 16

    %e0 = extract.v4f32 %reloaded, 0
    %e1 = extract.v4f32 %reloaded, 1
    %e2 = extract.v4f32 %reloaded, 2
    %e3 = extract.v4f32 %reloaded, 3
    %s01 = fadd.f32 %e0, %e1
    %s23 = fadd.f32 %e2, %e3
    %tot = fadd.f32 %s01, %s23
    %res = fptosi.i32 %tot [sat]
    ret %res
}
