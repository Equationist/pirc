target "aarch64-apple-darwin"

func @f() -> ptr {
.entry:
    %p = const.ptr 42
    ret %p
}
