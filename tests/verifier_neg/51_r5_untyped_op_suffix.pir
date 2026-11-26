target "aarch64-apple-darwin"

func @f() -> i32 {
.entry:
    fence.i128 [seq_cst]
    ret 0
}
