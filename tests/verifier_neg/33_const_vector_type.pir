target "aarch64-apple-darwin"

func @f() -> v4i32 cc "c" {
.entry:
    %v = const.v4i32 0
    ret %v
}
