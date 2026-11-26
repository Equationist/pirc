target "aarch64-apple-darwin"

@bad_vec = data v4i32 = { 1, 2 }, align 16

func @main() -> i32 {
.entry:
    ret 0
}
