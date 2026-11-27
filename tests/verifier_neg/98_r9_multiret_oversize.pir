target "x86_64-unknown-linux-gnu"
export func @foo(%out: ptr) -> void cc "c" multi_ret(i8, i8, size 16, align 1) {
.entry:
    ret
}
