target "aarch64-apple-darwin"

@g = global align 4 { zeroinit 0 }

func @main() -> i32 {
.entry:
    ret 0
}
