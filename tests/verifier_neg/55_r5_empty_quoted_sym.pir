target "aarch64-apple-darwin"

@"" = global i32 = 1, align 4

func @f() -> i32 {
.entry:
    ret 0
}
