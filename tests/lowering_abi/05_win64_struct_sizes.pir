// MODE: HIR
// EXPECT_SIG: @win_s1 (i8) -> i32
// EXPECT_SIG: @win_s2 (i16) -> i32
// EXPECT_SIG: @win_s3 (ptr) -> i32
// EXPECT_SIG: @win_s4 (i32) -> i32
// EXPECT_SIG: @win_s6 (ptr) -> i32
// EXPECT_SIG: @win_s8 (i64) -> i32
// EXPECT_SIG: @win_s12 (ptr) -> i32
// EXPECT_SIG: @win_s16 (ptr) -> i32
// EXPECT_SIG: @win_ret16 (ptr, i64) -> void
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "x86_64-pc-windows-msvc"

// Win64 rule: ONLY power-of-two sizes <= 8 bytes (1, 2, 4, 8) pass directly in registers.
type @S1  = { i8 }
type @S2  = { i16 }
type @S3  = { i8, i8, i8 }
type @S4  = { i32 }
type @S6  = { i16, i16, i16 }
type @S8  = { i32, i32 }
type @S12 = { i32, i32, i32 }
type @S16 = { i64, i64 }

extern func @win_s1(%p: byval(@S1)) -> i32 cc "c" [nounwind]
extern func @win_s2(%p: byval(@S2)) -> i32 cc "c" [nounwind]
extern func @win_s3(%p: byval(@S3)) -> i32 cc "c" [nounwind]
extern func @win_s4(%p: byval(@S4)) -> i32 cc "c" [nounwind]
extern func @win_s6(%p: byval(@S6)) -> i32 cc "c" [nounwind]
extern func @win_s8(%p: byval(@S8)) -> i32 cc "c" [nounwind]
extern func @win_s12(%p: byval(@S12)) -> i32 cc "c" [nounwind]
extern func @win_s16(%p: byval(@S16)) -> i32 cc "c" [nounwind]
extern func @win_ret16(%out: retbuf(@S16), %v: i64) -> void cc "c" [nounwind]

export func @driver() -> i32 cc "c" [nounwind] {
.entry:
    %a1 = alloca @S1
    %a2 = alloca @S2
    %a3 = alloca @S3
    %a4 = alloca @S4
    %a6 = alloca @S6
    %a8 = alloca @S8
    %a12 = alloca @S12
    %a16 = alloca @S16
    memset %a1, 0, 1, align 1
    memset %a2, 0, 2, align 2
    memset %a3, 0, 3, align 1
    memset %a4, 0, 4, align 4
    memset %a6, 0, 6, align 2
    memset %a8, 0, 8, align 4
    memset %a12, 0, 12, align 4
    call @win_ret16(%a16, 7)

    %r1 = call @win_s1(%a1)
    %r2 = call @win_s2(%a2)
    %r3 = call @win_s3(%a3)
    %r4 = call @win_s4(%a4)
    %r6 = call @win_s6(%a6)
    %r8 = call @win_s8(%a8)
    %r12 = call @win_s12(%a12)
    %r16 = call @win_s16(%a16)
    %s1 = add.i32 %r1, %r2
    %s2 = add.i32 %r3, %r4
    %s3 = add.i32 %r6, %r8
    %s4 = add.i32 %r12, %r16
    %t1 = add.i32 %s1, %s2
    %t2 = add.i32 %s3, %s4
    %res = add.i32 %t1, %t2
    ret %res
}
