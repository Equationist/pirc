target "x86_64-unknown-linux-gnu"
export @".mysec" = global section ".mysec" i32 = 1
export func @main() -> i32 { .entry: ret 0 }
