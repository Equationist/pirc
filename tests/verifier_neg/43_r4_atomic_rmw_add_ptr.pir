target "aarch64-apple-darwin"

func @f(%p: ptr, %v: ptr) -> ptr cc "c" {
.entry:
    %r = atomic_rmw.add.ptr %p, %v [seq_cst]
    ret %r
}
