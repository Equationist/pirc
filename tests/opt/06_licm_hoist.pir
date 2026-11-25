// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_RET: 85
// EXPECT_HOIST_BEFORE: .loop mul.i32
// EXPECT_HOIST_BEFORE: .loop load.i32
target "aarch64-apple-darwin"

// Pure loop-invariant math and unconditional noalias invariant loads must hoist to the loop preheader.
func @sum_scaled(%p: ptr noalias, %a: i32, %b: i32, %n: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(0, 0)

.loop(%i: i32, %acc: i32):
    // Both ops are loop-invariant and execute unconditionally on every iteration
    // (no stores in loop; %p is noalias).
    %prod = mul.i32 %a, %b
    %bias = load.i32 %p, align 4
    %step = add.i32 %prod, %bias
    %acc1 = add.i32 %acc, %step
    %i1 = add.i32 %i, 1
    %more = cmp_lt_s.i32 %i1, %n
    br %more, .loop(%i1, %acc1), .exit(%acc1)

.exit(%res: i32):
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %slot = alloca.ptr 4, align 4
    store.i32 5, %slot, align 4
    // 5 iterations * (3 * 4 + 5) = 5 * 17 = 85
    %ans = call @sum_scaled(%slot, 3, 4, 5)
    ret %ans
}
