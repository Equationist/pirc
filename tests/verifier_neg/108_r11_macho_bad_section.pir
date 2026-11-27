target "aarch64-apple-darwin"
@g = global section ",__data" i32 = 1
export func @main() -> i32 { .entry: ret 0 }
