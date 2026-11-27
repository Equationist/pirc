// Reject non-zero initializer placed in ELF nobits .bss section
target "x86_64-unknown-linux-gnu"

@b = global section ".bss" i64 = 1, align 8

export func @f() -> i64 {
.entry:
    ret 0
}
