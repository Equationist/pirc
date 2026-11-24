// MODE: HIR
// EXPECT_LIR_CONTAINS: @__pir_exc_head
// EXPECT_LIR_CONTAINS: @__pir_setjmp
// EXPECT_LIR_CONTAINS: @__pir_unwind_resume
// EXPECT_LIR_FORBIDS: invoke
// EXPECT_LIR_FORBIDS: resume
target "aarch64-apple-darwin"

extern func @risky(%x: i32) -> i32 cc "c"

export func @with_landing_pad(%x: i32) -> i32 cc "c" {
.entry:
    invoke @risky(%x) then .ok(ret) unwind .catch(exn)

.ok(%v: i32):
    ret %v

.catch(%e: ptr):
    resume %e
}
