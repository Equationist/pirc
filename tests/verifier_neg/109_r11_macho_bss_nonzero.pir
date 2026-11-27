target "aarch64-apple-darwin"
@g = global section "__DATA,__bss" i32 = 42
export func @main() -> i32 { .entry: ret 0 }
