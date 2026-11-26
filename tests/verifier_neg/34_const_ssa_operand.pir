target "aarch64-apple-darwin"

func @f(%x: i32) -> i32 cc "c" {
.entry:
    %c = const.i32 %x
    ret %c
}
