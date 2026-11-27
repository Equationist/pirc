// Reject trailing identifier characters glued onto numeric local name
target "aarch64-apple-darwin"

export func @f(%0: i64) -> i64 {
.entry:
    ret %0force_stack
}
