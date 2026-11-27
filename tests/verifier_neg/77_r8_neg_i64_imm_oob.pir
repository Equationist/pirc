// Reject negative i64 immediate below INT64_MIN
target "aarch64-apple-darwin"

export func @f() -> i64 {
.entry:
    %x = const.i64 -9223372036854775809
    ret %x
}
