target "aarch64-apple-darwin"

func @f(%x: i32) -> f64 cc "c" {
.entry:
    %r = bitcast.f64 %x
    ret %r
}
