target "aarch64-apple-darwin"

func @f() -> i32 {
.entry:
    %p = alloca.ptr -16, align 8
    ret 0
}
