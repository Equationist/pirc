target "aarch64-apple-darwin"

extern @g = global i32 = 42, align 4

func @f() -> i32 {
.entry:
    ret 0
}
