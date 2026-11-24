// MODE: HIR
// EXPECT_SIG: @take_packed_unaligned (ptr) -> i64
// EXPECT_SIG: @take_big_24b (ptr) -> i64
// EXPECT_SIG: @ret_big_24b (ptr, i64) -> void
// EXPECT_SIG: @spill_all_or_nothing (i64, i64, i64, i64, i64, ptr) -> i64
// EXPECT_LIR_FORBIDS: byval
// EXPECT_LIR_FORBIDS: retbuf
target "x86_64-unknown-linux-gnu"

// Unaligned field -> SysV AMD64 classifies as MEMORY
type @Packed9 = packed { i8, i64 }
// > 16 bytes -> SysV AMD64 classifies as MEMORY (sret for retbuf)
type @Big24   = { i64, i64, i8 }
// 16 bytes needing 2 GP regs; when 5 GP regs already used (1 left), must spill ENTIRELY to MEMORY
type @TwoI64  = { i64, i64 }

extern func @take_packed_unaligned(%p: byval(@Packed9)) -> i64 cc "c" [nounwind]
extern func @take_big_24b(%p: byval(@Big24)) -> i64 cc "c" [nounwind]
extern func @ret_big_24b(%out: retbuf(@Big24), %seed: i64) -> void cc "c" [nounwind]
extern func @spill_all_or_nothing(%a: i64, %b: i64, %c: i64, %d: i64, %e: i64, %p: byval(@TwoI64)) -> i64 cc "c" [nounwind]

export func @driver() -> i64 cc "c" [nounwind] {
.entry:
    %p9 = alloca @Packed9
    %b24 = alloca @Big24
    %t16 = alloca @TwoI64
    memset %p9, 0, 9, align 1
    memset %t16, 0, 16, align 8
    call @ret_big_24b(%b24, 42)
    %r1 = call @take_packed_unaligned(%p9)
    %r2 = call @take_big_24b(%b24)
    %r3 = call @spill_all_or_nothing(1, 2, 3, 4, 5, %t16)
    %s1 = add.i64 %r1, %r2
    %s2 = add.i64 %s1, %r3
    ret %s2
}
