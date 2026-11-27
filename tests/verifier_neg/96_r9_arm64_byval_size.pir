target "aarch64-apple-darwin"
export func @foo(%p: ptr byval_size 16) -> void cc "c" {
.entry:
    ret
}
