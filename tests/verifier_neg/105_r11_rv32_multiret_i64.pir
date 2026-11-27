target "riscv32-unknown-linux-gnu"
export func @f(%out: ptr) -> void cc "c" multi_ret(i64, i64, size 16, align 8) {
.entry:
    ret
}
export func @main() -> i32 { .entry: ret 0 }
