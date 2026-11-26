target "aarch64-apple-darwin"

func @f(%p: ptr) -> i32 {
.entry:
    %v = load.i32 %p, align 0
    ret %v
}
