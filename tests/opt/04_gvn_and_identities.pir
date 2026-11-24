// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_LIR_FORBIDS: xor.i32
// EXPECT_LIR_FORBIDS: select.i32
// EXPECT_LIR_FORBIDS: mul.i32
// EXPECT_LIR_CONTAINS: shl.i32
target "aarch64-apple-darwin"

// GVN must eliminate redundant dominating expressions and fold safe algebraic identities.
export func @gvn_and_peepholes(%x: i32, %y: i32) -> i32 cc "c" [nounwind] {
.entry:
    %sum1 = add.i32 %x, %y
    %zero = xor.i32 %x, %x
    %plus0 = add.i32 %sum1, %zero
    %scaled = mul.i32 %plus0, 8
    %picked = select.i32 1, %scaled, %y
    jmp .next(%picked)

.next(%acc: i32):
    // Redundant expression dominated by .entry (%sum1 = add.i32 %x, %y)
    %sum2 = add.i32 %x, %y
    %res = add.i32 %acc, %sum2
    ret %res
}
