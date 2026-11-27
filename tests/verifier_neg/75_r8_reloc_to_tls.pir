// Reject static relocation taking address of a TLS global
target "aarch64-apple-darwin"

@tls_x = tls i64 = 1, align 8
@bad = global ptr = @tls_x, align 8

export func @f() -> i64 {
.entry:
    ret 0
}
