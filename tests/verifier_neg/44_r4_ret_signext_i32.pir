target "aarch64-apple-darwin"

func @f() -> i32 signext cc "c" {
.entry:
    ret 0
}
