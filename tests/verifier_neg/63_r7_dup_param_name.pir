target "aarch64-apple-darwin"

func @dup_param(%x: i32, %x: i32) -> i32 {
.entry:
    ret %x
}
