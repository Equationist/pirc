target "aarch64-apple-darwin"
@g = global zeroinit 0, align 4
export func @main() -> i32 { .entry: ret 0 }
