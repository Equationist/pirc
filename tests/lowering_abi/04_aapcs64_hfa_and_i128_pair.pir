// MODE: HIR
// EXPECT_SIG: @take_hfa4 (f32, f32, f32, f32) -> f32
// EXPECT_SIG: @take_non_hfa5 (ptr) -> f32
// EXPECT_SIG: @ret_non_hfa5 (ptr, f32) -> void
// EXPECT_SIG: @take_i32_then_i128 (i32, i64, i64, i64) -> i64
// EXPECT_LIR_FORBIDS: i128
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "aarch64-unknown-linux-gnu"

// 4-element Homogeneous Floating-Point Aggregate (HFA) -> 4 separate f32 regs (s0..s3)
type @Hfa4 = { f32, f32, f32, f32 }
// 5-element float struct exceeds HFA max of 4 -> indirect ptr / x8 retbuf
type @NonHfa5 = { f32, f32, f32, f32, f32 }

extern func @take_hfa4(%h: byval(@Hfa4)) -> f32 cc "c" [nounwind]
extern func @take_non_hfa5(%h: byval(@NonHfa5)) -> f32 cc "c" [nounwind]
extern func @ret_non_hfa5(%out: retbuf(@NonHfa5), %v: f32) -> void cc "c" [nounwind]
// AAPCS64 even-register-pair alignment for i128: (x0=i32, pad x1, x2:x3=i128)
extern func @take_i32_then_i128(%tag: i32, %big: i128) -> i64 cc "c" [nounwind]

export func @driver(%tag: i32, %big: i128) -> i64 cc "c" [nounwind] {
.entry:
    %h4 = alloca @Hfa4
    %h5 = alloca @NonHfa5
    memset %h4, 0, 16, align 4
    call @ret_non_hfa5(%h5, 1.5)
    %f1 = call @take_hfa4(%h4)
    %f2 = call @take_non_hfa5(%h5)
    %r = call @take_i32_then_i128(%tag, %big)
    ret %r
}
