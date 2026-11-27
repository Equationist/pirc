// Reject lowered C ABI return metadata on non-C calling convention
target "aarch64-apple-darwin"

export func @f(%out: ptr) -> void cc "pir" sret_x8 {
.entry:
    ret
}
