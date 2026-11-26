target "aarch64-apple-darwin"

type @Pad8 = pad 8

func @f() -> i32 {
.entry:
    %p = alloca @Pad8
    ret 0
}
