// Reject duplicate parameter attribute signext
target "aarch64-apple-darwin"

export func @f(%a: i8 signext signext) -> i32 cc "c" {
.entry:
    %r = sext.i32 %a
    ret %r
}
