// Reject scalar integer immediate in vector bitcast
target "aarch64-apple-darwin"

export func @f() -> i32 {
.entry:
    %v = bitcast.v4i32 42
    %e = extract.v4i32 %v, 0
    ret %e
}
