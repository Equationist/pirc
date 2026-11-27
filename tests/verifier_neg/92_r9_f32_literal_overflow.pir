target "x86_64-unknown-linux-gnu"
@g = global f32 = 1e40, align 4
export func @main() -> i32 { .entry: ret 0 }
