// MODE: LIR
target "x86_64-unknown-linux-gnu"
export func @f(%p: ptr byval_size 1073741824) -> i32 cc "c" {
.entry:
    ret 0
}
export func @main() -> i32 { .entry: ret 0 }
