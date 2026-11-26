target "aarch64-apple-darwin"

func @f() -> f64 {
.entry:
    %x = const.f64 0x1.0
    ret %x
}
