target "aarch64-apple-darwin"

func @huge_alloca_ptr() -> ptr {
.entry:
    %p = alloca.ptr 2000000000000, align 8
    ret %p
}
