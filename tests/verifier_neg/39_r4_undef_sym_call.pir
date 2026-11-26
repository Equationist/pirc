target "aarch64-apple-darwin"

func @f() -> i32 cc "c" {
.entry:
    %r = call @nonexistent_sym()
    ret %r
}
