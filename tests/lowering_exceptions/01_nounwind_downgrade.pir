// MODE: HIR
// EXPECT_LIR_FORBIDS: invoke
// EXPECT_LIR_FORBIDS: resume
// EXPECT_LIR_FORBIDS: @__pir_setjmp
// EXPECT_LIR_FORBIDS: @__pir_exc_head
target "aarch64-apple-darwin"

sig @SafeSig = (i32) -> i32 cc "c" [nounwind]
extern func @safe_fn(%x: i32) -> i32 cc "c" [nounwind]

export func @both_downgrade(%x: i32, %cb: ptr) -> i32 cc "c" {
.entry:
    // Downgrade 1: direct invoke targeting a callee declared [nounwind]
    invoke @safe_fn(%x) then .next(ret) unwind .dead1(exn)

.next(%v1: i32):
    // Downgrade 2: indirect invoke targeting a signature declared [nounwind]
    invoke_indirect @SafeSig, %cb(%v1) then .done(ret) unwind .dead2(exn)

.done(%v2: i32):
    ret %v2

.dead1(%e1: ptr):
    resume %e1

.dead2(%e2: ptr):
    resume %e2
}
