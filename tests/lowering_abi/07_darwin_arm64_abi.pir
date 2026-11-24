// MODE: HIR
// EXPECT_SIG: @darwin_i32_then_i128 (i32, i64, i64) -> i64
// EXPECT_SIG: @take_hfa2f (f32, f32) -> f32
// EXPECT_SIG: @take_hfa4f (f32, f32, f32, f32) -> f32
// EXPECT_SIG: @take_hfa2d (f64, f64) -> f64
// EXPECT_SIG: @take_hva2 (v4f32, v4f32) -> v4f32
// EXPECT_SIG: @take_non_hfa5 (ptr) -> f32
// EXPECT_SIG: @ret_non_hfa5 (ptr, f32) -> void
// EXPECT_SIG: @take_int_pair (i64) -> i32
// EXPECT_SIG: @ret_int_pair (i32, i32) -> i64
// EXPECT_SIG: @take_mixed_8b (i64) -> i32
// EXPECT_SIG: @take_mixed_16b (i64, i64) -> i64
// EXPECT_SIG: @take_packed_9b (i64, i64) -> i64
// EXPECT_SIG: @take_big_24b (ptr) -> i64
// EXPECT_SIG: @ret_big_24b (ptr, i64) -> void
// EXPECT_SIG: @take_simd_wrap (v4f32) -> v4f32
// EXPECT_SIG: @ret_simd_wrap (v4f32) -> v4f32
// EXPECT_LIR_FORBIDS: i128
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "aarch64-apple-darwin"

// Homogeneous Floating-Point / Short-Vector Aggregates (HFA/HVA): up to 4 FP/SIMD elements
type @Hfa2F    = { f32, f32 }
type @Hfa4F    = { f32, f32, f32, f32 }
type @Hfa2D    = { f64, f64 }
type @Hva2     = { v4f32, v4f32 }
type @SimdWrap = { v4f32 }
// 5-element float struct exceeds HFA max of 4 -> indirect ptr / x8 retbuf
type @NonHfa5  = { f32, f32, f32, f32, f32 }

// Non-HFA composites <= 16B pass in GP regs x0..x7 (even if mixed int/float or packed)
type @IntPair  = { i32, i32 }
type @Mixed8   = { i32, f32 }
type @Mixed16  = { i64, f64 }
type @Packed9  = packed { i8, i64 }
// > 16B non-HFA composite -> indirect ptr / x8 retbuf
type @Big24    = { i64, i64, i8 }

// Darwin ARM64 does NOT require even-register alignment for i128: (w0=i32, x1:x2=i128)
extern func @darwin_i32_then_i128(%tag: i32, %big: i128) -> i64 cc "c" [nounwind]
extern func @take_hfa2f(%h: byval(@Hfa2F)) -> f32 cc "c" [nounwind]
extern func @take_hfa4f(%h: byval(@Hfa4F)) -> f32 cc "c" [nounwind]
extern func @take_hfa2d(%h: byval(@Hfa2D)) -> f64 cc "c" [nounwind]
extern func @take_hva2(%h: byval(@Hva2)) -> v4f32 cc "c" [nounwind]
extern func @take_non_hfa5(%h: byval(@NonHfa5)) -> f32 cc "c" [nounwind]
extern func @ret_non_hfa5(%out: retbuf(@NonHfa5), %v: f32) -> void cc "c" [nounwind]
extern func @take_int_pair(%p: byval(@IntPair)) -> i32 cc "c" [nounwind]
extern func @ret_int_pair(%out: retbuf(@IntPair), %a: i32, %b: i32) -> void cc "c" [nounwind]
extern func @take_mixed_8b(%p: byval(@Mixed8)) -> i32 cc "c" [nounwind]
extern func @take_mixed_16b(%p: byval(@Mixed16)) -> i64 cc "c" [nounwind]
extern func @take_packed_9b(%p: byval(@Packed9)) -> i64 cc "c" [nounwind]
extern func @take_big_24b(%p: byval(@Big24)) -> i64 cc "c" [nounwind]
extern func @ret_big_24b(%out: retbuf(@Big24), %v: i64) -> void cc "c" [nounwind]
extern func @take_simd_wrap(%w: byval(@SimdWrap)) -> v4f32 cc "c" [nounwind]
extern func @ret_simd_wrap(%out: retbuf(@SimdWrap), %v: v4f32) -> void cc "c" [nounwind]

export func @driver(%tag: i32, %big: i128, %v: v4f32) -> i64 cc "c" [nounwind] {
.entry:
    %h2f = alloca @Hfa2F
    %h4f = alloca @Hfa4F
    %h2d = alloca @Hfa2D
    %hv2 = alloca @Hva2, align 16
    %nh5 = alloca @NonHfa5
    %ip  = alloca @IntPair
    %m8  = alloca @Mixed8
    %m16 = alloca @Mixed16
    %p9  = alloca @Packed9
    %b24 = alloca @Big24
    %sw  = alloca @SimdWrap, align 16

    memset %h2f, 0, 8, align 4
    memset %h4f, 0, 16, align 4
    memset %h2d, 0, 16, align 8
    %hv2_0 = field_ptr @Hva2, %hv2, 0
    %hv2_1 = field_ptr @Hva2, %hv2, 1
    store.v4f32 %v, %hv2_0, align 16
    store.v4f32 %v, %hv2_1, align 16
    memset %m8, 0, 8, align 4
    memset %m16, 0, 16, align 8
    memset %p9, 0, 9, align 1

    call @ret_non_hfa5(%nh5, 1.5)
    call @ret_int_pair(%ip, 1, 2)
    call @ret_big_24b(%b24, 42)
    call @ret_simd_wrap(%sw, %v)

    %r0 = call @darwin_i32_then_i128(%tag, %big)
    %f1 = call @take_hfa2f(%h2f)
    %f2 = call @take_hfa4f(%h4f)
    %d1 = call @take_hfa2d(%h2d)
    %v1 = call @take_hva2(%hv2)
    %f3 = call @take_non_hfa5(%nh5)
    %i1 = call @take_int_pair(%ip)
    %i2 = call @take_mixed_8b(%m8)
    %l1 = call @take_mixed_16b(%m16)
    %l2 = call @take_packed_9b(%p9)
    %l3 = call @take_big_24b(%b24)
    %v2 = call @take_simd_wrap(%sw)

    %e1 = extract.v4f32 %v1, 0
    %e2 = extract.v4f32 %v2, 0
    %fs1 = fadd.f32 %f1, %f2
    %fs2 = fadd.f32 %fs1, %f3
    %fs3 = fadd.f32 %fs2, %e1
    %fs4 = fadd.f32 %fs3, %e2
    %fi = fptosi.i64 %fs4 [sat]
    %di = fptosi.i64 %d1 [sat]
    %i1_64 = sext.i64 %i1
    %i2_64 = sext.i64 %i2

    %s1 = add.i64 %r0, %fi
    %s2 = add.i64 %s1, %di
    %s3 = add.i64 %s2, %i1_64
    %s4 = add.i64 %s3, %i2_64
    %s5 = add.i64 %s4, %l1
    %s6 = add.i64 %s5, %l2
    %res = add.i64 %s6, %l3
    ret %res
}
