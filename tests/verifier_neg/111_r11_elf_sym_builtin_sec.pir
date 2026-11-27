target "x86_64-unknown-linux-gnu"
export @".text" = global i64 = 1, align 8
export func @main() -> i32 { .entry: ret 0 }
