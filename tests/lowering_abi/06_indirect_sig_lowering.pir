// MODE: HIR
// EXPECT_LIR_CONTAINS: sig @PairFn = (i64) -> i64 cc "c"
// EXPECT_LIR_CONTAINS: sig @Hfa2Fn = (f32, f32) -> f32 cc "c"
// EXPECT_LIR_CONTAINS: sig @BigFn = (ptr, ptr) -> void cc "c"
// EXPECT_LIR_FORBIDS: type @
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "aarch64-apple-darwin"

type @Pair  = { i32, i32 }
type @Vec2F = { f32, f32 }
type @Big   = { i64, i64, i64 }

sig @PairFn = (retbuf(@Pair), byval(@Pair)) -> void cc "c" [nounwind]
sig @Hfa2Fn = (byval(@Vec2F)) -> f32 cc "c" [nounwind]
sig @BigFn  = (retbuf(@Big), byval(@Big)) -> void cc "c" [nounwind]

export func @invoke_callbacks(%f1: ptr, %f2: ptr, %f3: ptr) -> i32 cc "c" [nounwind] {
.entry:
    %p_in = alloca @Pair
    %p_out = alloca @Pair
    %v_in = alloca @Vec2F
    %b_in = alloca @Big
    %b_out = alloca @Big
    memset %p_in, 0, 8, align 4
    memset %v_in, 0, 8, align 4
    memset %b_in, 0, 24, align 8

    call_indirect @PairFn, %f1(%p_out, %p_in)
    %fv = call_indirect @Hfa2Fn, %f2(%v_in)
    call_indirect @BigFn, %f3(%b_out, %b_in)

    %px = field_ptr @Pair, %p_out, 0
    %v = load.i32 %px, align 4
    %fi = fptosi.i32 %fv [sat]
    %res = add.i32 %v, %fi
    ret %res
}
