// Reject [wrap] flag on unsigned division div_u
target "aarch64-apple-darwin"

export func @f(%a: i64, %b: i64) -> i64 {
.entry:
    %q = div_u.i64 %a, %b [wrap]
    ret %q
}
