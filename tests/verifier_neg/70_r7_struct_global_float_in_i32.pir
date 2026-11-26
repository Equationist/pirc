target "aarch64-apple-darwin"

type @S = { i32, i32 }
@bad = data @S = { 1.5, 0 }, align 4

func @main() -> i32 {
.entry:
    ret 0
}
