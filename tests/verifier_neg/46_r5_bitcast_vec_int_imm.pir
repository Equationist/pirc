target "aarch64-apple-darwin"

func @f() -> i32 {
.entry:
    %v = bitcast.v4i32 42
    %x = extract.v4i32 %v, 0
    ret %x
}
