// MODE: HIR
// EXPECT_SIG: @take_int_pair (i64) -> i32
// EXPECT_SIG: @take_flt_pair (v2f32) -> f32
// EXPECT_SIG: @take_mixed_8b (i64) -> i32
// EXPECT_SIG: @take_mixed_16b (i64, f64) -> i64
// EXPECT_SIG: @ret_int_pair (i32, i32) -> i64
// EXPECT_SIG: @ret_flt_pair (f32, f32) -> v2f32
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "x86_64-unknown-linux-gnu"

type @IntPair  = { i32, i32 }
type @FltPair  = { f32, f32 }
type @Mixed8   = { i32, f32 }
type @Mixed16  = { i64, f64 }

extern func @take_int_pair(%p: byval(@IntPair)) -> i32 cc "c" [nounwind]
extern func @take_flt_pair(%p: byval(@FltPair)) -> f32 cc "c" [nounwind]
extern func @take_mixed_8b(%p: byval(@Mixed8)) -> i32 cc "c" [nounwind]
extern func @take_mixed_16b(%p: byval(@Mixed16)) -> i64 cc "c" [nounwind]
extern func @ret_int_pair(%out: retbuf(@IntPair), %a: i32, %b: i32) -> void cc "c" [nounwind]
extern func @ret_flt_pair(%out: retbuf(@FltPair), %x: f32, %y: f32) -> void cc "c" [nounwind]

export func @driver() -> i32 cc "c" [nounwind] {
.entry:
    %ip = alloca @IntPair
    %fp = alloca @FltPair
    %m8 = alloca @Mixed8
    %m16 = alloca @Mixed16
    memset %m8, 0, 8, align 4
    memset %m16, 0, 16, align 8

    call @ret_int_pair(%ip, 1, 2)
    call @ret_flt_pair(%fp, 1.0, 2.0)
    %r1 = call @take_int_pair(%ip)
    %r2 = call @take_flt_pair(%fp)
    %r3 = call @take_mixed_8b(%m8)
    %r4 = call @take_mixed_16b(%m16)
    %r4_32 = trunc.i32 %r4
    %s1 = add.i32 %r1, %r3
    %s2 = add.i32 %s1, %r4_32
    ret %s2
}
