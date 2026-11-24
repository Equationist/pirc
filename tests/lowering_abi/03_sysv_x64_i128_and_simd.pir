// MODE: HIR
// EXPECT_SIG: @c_i128_add (i64, i64, i64, i64) -> i64
// EXPECT_SIG: @c_v4f32_dot (v4f32, v4f32) -> f32
// EXPECT_SIG: @c_v4f32_wrap (v4f32) -> f32
// EXPECT_LIR_FORBIDS: i128
// EXPECT_LIR_FORBIDS: byval
target "x86_64-unknown-linux-gnu"

type @SimdWrap = { v4f32 }

extern func @c_i128_add(%a: i128, %b: i128) -> i64 cc "c" [nounwind]
extern func @c_v4f32_dot(%a: v4f32, %b: v4f32) -> f32 cc "c" [nounwind]
extern func @c_v4f32_wrap(%w: byval(@SimdWrap)) -> f32 cc "c" [nounwind]

export func @driver(%a: i128, %b: i128, %v: v4f32) -> i64 cc "c" [nounwind] {
.entry:
    %w = alloca @SimdWrap, align 16
    %wf = field_ptr @SimdWrap, %w, 0
    store.v4f32 %v, %wf, align 16
    %r1 = call @c_i128_add(%a, %b)
    %r2 = call @c_v4f32_dot(%v, %v)
    %r3 = call @c_v4f32_wrap(%w)
    %sum = fadd.f32 %r2, %r3
    %si = fptosi.i64 %sum [sat]
    %out = add.i64 %r1, %si
    ret %out
}
