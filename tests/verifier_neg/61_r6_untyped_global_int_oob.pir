target "aarch64-apple-darwin"

@g = data align 4 { 300 }

func @main() -> i32 {
.entry:
    ret 0
}
