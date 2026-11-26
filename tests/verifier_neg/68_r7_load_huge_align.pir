target "aarch64-apple-darwin"

func @huge_align(%p: ptr) -> i32 {
.entry:
    %v = load.i32 %p, align 2147483648
    ret %v
}
