// Reject call-site ABI metadata on branch target arguments
target "aarch64-apple-darwin"

export func @f(%x: i64) -> i64 {
.entry:
    jmp .next(%x force_stack)
.next(%y: i64):
    ret %y
}
