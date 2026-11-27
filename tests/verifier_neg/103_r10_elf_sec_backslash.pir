target "x86_64-unknown-linux-gnu"
@g = global section ".data\",\"aw\"" i32 = 1, align 4
export func @main() -> i32 { .entry: ret 0 }
