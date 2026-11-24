// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_RET: 10
// EXPECT_IN_LOOP: load.i32
// EXPECT_IN_LOOP: div_s.i32
target "aarch64-apple-darwin"

// LICM safety:
// 1. Load from %p may alias store to %q -> MUST remain inside loop.
func @aliased_loop(%p: ptr, %q: ptr, %n: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(1, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_gt_s.i32 %i, %n
    br %done, .exit(%acc), .body

.body:
    store.i32 %i, %q, align 4
    %v = load.i32 %p, align 4
    %acc1 = add.i32 %acc, %v
    %i1 = add.i32 %i, 1
    jmp .loop(%i1, %acc1)

.exit(%res: i32):
    ret %res
}

// 2. Trapping op inside a potentially 0-trip loop MUST NOT be hoisted to preheader.
func @guarded_div_loop(%n: i32, %d: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(0, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit(%acc), .body

.body:
    %q = div_s.i32 100, %d [trap]
    %acc1 = add.i32 %acc, %q
    %i1 = add.i32 %i, 1
    jmp .loop(%i1, %acc1)

.exit(%res: i32):
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %slot = alloca.ptr 4, align 4
    store.i32 0, %slot, align 4
    // %p == %q: loads see 1, 2, 3, 4 -> sum = 10
    %s1 = call @aliased_loop(%slot, %slot, 4)
    // 0-trip loop with divisor 0: must NOT trap!
    %s2 = call @guarded_div_loop(0, 0)
    %res = add.i32 %s1, %s2
    ret %res
}
