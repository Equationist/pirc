target "aarch64-apple-darwin"
export func @main() -> i32 {
.entry:
    %x = bitcast.i32 1e50
    ret %x
}
