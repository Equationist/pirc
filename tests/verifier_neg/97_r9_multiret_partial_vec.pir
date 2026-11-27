target "aarch64-apple-darwin"
export func @foo(%out: ptr) -> void cc "c" multi_ret(v4f32, v4f32, size 24, align 16) {
.entry:
    ret
}
