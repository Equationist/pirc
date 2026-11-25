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

// 3. Noalias (`restrict`) pointer load inside a conditional/0-trip loop MUST NOT
//    be speculatively hoisted (p may be NULL!).
func @guarded_noalias_null(%p: ptr noalias, %n: i32) -> i32 [nounwind] {
.entry:
    jmp .loop(0, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit(%acc), .body

.body:
    %pi = ptrtoint.i64 %p
    %nn = cmp_ne.i64 %pi, 0
    br %nn, .load_blk, .skip_blk

.load_blk:
    %v = load.i32 %p, align 4
    %a1 = add.i32 %acc, %v
    jmp .latch(%a1)

.skip_blk:
    jmp .latch(%acc)

.latch(%an: i32):
    %i1 = add.i32 %i, 1
    jmp .loop(%i1, %an)

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
    // NULL noalias pointer guarded inside loop: must NOT segfault!
    %z = const.i64 0
    %np = inttoptr.ptr %z
    %s3 = call @guarded_noalias_null(%np, 4)
    %r1 = add.i32 %s1, %s2
    %res = add.i32 %r1, %s3
    ret %res
}
