// MODE: LIR
// EXPECT_ERR: LIR forbids HIR-only 'invoke'
target "x86_64-unknown-linux-gnu"

extern func @f() -> i32 cc "c"

func @bad() -> i32 {
.entry:
    invoke @f() then .ok(ret) unwind .err(exn)
.ok(%v: i32):
    ret %v
.err(%e: ptr):
    resume %e
}
