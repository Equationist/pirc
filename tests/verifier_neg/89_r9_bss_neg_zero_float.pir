target "x86_64-unknown-linux-gnu"
@f = global section ".bss" f32 = -0, align 4
export func @main() -> i32 { .entry: ret 0 }
