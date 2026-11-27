target "aarch64-apple-darwin"
export func @f() -> i32 section "__DATA,__mybss,zerofill" { .entry: ret 1 }
export func @main() -> i32 { .entry: ret 0 }
