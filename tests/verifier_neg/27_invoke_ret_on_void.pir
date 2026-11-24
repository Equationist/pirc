// MODE: HIR
// EXPECT_ERR: 'ret' placeholder only valid in non-void invoke 'then' edge
target "x86_64-unknown-linux-gnu"

extern func @vthrow() -> void cc "c"

func @bad() -> i32 {
.entry:
    invoke @vthrow() then .ok(ret) unwind .err(exn)
.ok(%v: i32):
    ret %v
.err(%e: ptr):
    resume %e
}
