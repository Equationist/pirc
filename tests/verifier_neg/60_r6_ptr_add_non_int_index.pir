target "aarch64-apple-darwin"

func @f(%p: ptr, %idx: f64) -> ptr {
.entry:
    %q = ptr_add.i64 %p, %idx
    ret %q
}
