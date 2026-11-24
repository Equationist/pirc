// MODE: HIR
// EXPECT_RET: 73
target "aarch64-apple-darwin"

// Regression test (exposed by Ada 83 invoke + inlined mutator):
// When a small callee mutates a caller stack slot and then executes `resume`,
// inlining the callee into the caller's `invoke` (.exc_call) block must NOT
// allow mem2reg to promote that stack slot across the hidden setjmp/longjmp
// unwind edge and drop the store before `resume`!

@g_tag = data align 1 { "STEP_ERR\0" }

func @mutate_and_throw(%p: ptr, %do_throw: i8) -> void {
.entry:
    %v = load.i32 %p, align 4
    %nv = add.i32 %v, 17
    store.i32 %nv, %p, align 4
    br %do_throw, .throw, .ok

.throw:
    %t = addr @g_tag
    resume %t

.ok:
    ret
}

export func @main() -> i32 cc "c" {
.entry:
    %slot = alloca i32, align 4
    store.i32 5, %slot, align 4
    jmp .loop(1)

.loop(%i: i32):
    %t = cmp_eq.i32 %i, 4
    invoke @mutate_and_throw(%slot, %t) then .next(%i) unwind .caught(exn)

.next(%i2: i32):
    %done = cmp_eq.i32 %i2, 10
    %i3 = add.i32 %i2, 1
    br %done, .exit, .loop(%i3)

.caught(%exn: ptr):
    // Expected: 5 + 4*17 = 73 (store in iteration 4 happened BEFORE resume!)
    %res = load.i32 %slot, align 4
    ret %res

.exit:
    ret 0
}
