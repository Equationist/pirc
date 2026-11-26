target "aarch64-apple-darwin"

func @f() -> ptr cc "c" {
.entry:
    %p = alloca_dyn i8, 1.5, align 8
    ret %p
}
