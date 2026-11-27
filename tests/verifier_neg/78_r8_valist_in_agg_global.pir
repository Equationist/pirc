// Reject aggregate global containing va_list
target "aarch64-apple-darwin"

type @HasVL = { i64, va_list }
@g = global @HasVL = zeroinit, align 8

export func @f() -> i64 {
.entry:
    ret 0
}
