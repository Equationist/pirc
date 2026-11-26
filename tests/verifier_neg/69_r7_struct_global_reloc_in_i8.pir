target "aarch64-apple-darwin"

type @S = { i8, i64 }
@tgt = data i32 = 1, align 4
@bad = data @S = { @tgt, 0 }, align 8

func @main() -> i32 {
.entry:
    ret 0
}
