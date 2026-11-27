target "aarch64-apple-darwin"
type @S = { i64, i64 }
export func @foo(%out: ptr, %a: byval(@S)) -> void cc "c" multi_ret(i64, i64, size 16, align 8) {
.entry:
    ret
}
