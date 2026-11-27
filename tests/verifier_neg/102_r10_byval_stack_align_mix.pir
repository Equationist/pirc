target "aarch64-apple-darwin"
type @S = { i64, i64 }
func @f(%p: byval(@S) stack_align 16) -> void cc "c" {
.entry:
    ret
}
export func @main() -> i32 { .entry: ret 0 }
