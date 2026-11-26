target "aarch64-apple-darwin"

extern func @ext_f(i32, ...) -> void cc "c"

func @f(%x: i64) -> void cc "c" {
.entry:
    call @ext_f(0, %x: f64)
    ret
}
