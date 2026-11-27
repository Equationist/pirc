// Reject duplicate instruction flags [wrap, wrap]
target "aarch64-apple-darwin"

export func @f(%a: i64, %b: i64) -> i64 {
.entry:
    %r = add.i64 %a, %b [wrap, wrap]
    ret %r
}
