// Reject cyclic aggregate type with braced global initializer cleanly (no stack overflow)
target "aarch64-apple-darwin"

type @Cyc = { @Cyc }
@g = global @Cyc = { 0 }, align 1

export func @f() -> i64 {
.entry:
    ret 0
}
