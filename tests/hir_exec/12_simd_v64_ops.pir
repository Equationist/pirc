// MODE: HIR
// TEST_SIMD_CAPS: 128, 0
// EXPECT_RET: 36
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 64-bit vectors: v2f32, v2i32, v4i16, v8i8
    %slot = alloca v2f32, align 8
    %f = splat.v2f32 3.5
    %f1 = insert.v2f32 %f, 6.5, 1
    store.v2f32 %f1, %slot, align 8
    %f_ld = load.v2f32 %slot, align 8
    %f_sw = shuffle.v2f32 %f_ld, %f_ld [1, 0]
    %fa = extract.v2f32 %f_sw, 0
    %fb = extract.v2f32 %f_sw, 1
    %fsum = fadd.f32 %fa, %fb
    // 6.5 + 3.5 = 10
    %fi = fptosi.i32 %fsum [sat]

    %w1 = splat.v4i16 4
    %w2 = splat.v4i16 5
    %wm = mul.v4i16 %w1, %w2
    %w_lane = extract.v4i16 %wm, 2
    %wi = zext.i32 %w_lane
    // 20

    %b1 = splat.v8i8 3
    %b2 = splat.v8i8 3
    %bs = add.v8i8 %b1, %b2
    %bl = extract.v8i8 %bs, 7
    %bi = zext.i32 %bl
    // 6

    // 10 + 20 + 6 = 36
    %t = add.i32 %fi, %wi
    %res = add.i32 %t, %bi
    ret %res
}
