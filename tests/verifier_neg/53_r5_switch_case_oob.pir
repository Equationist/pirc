target "aarch64-apple-darwin"

func @f(%x: i8) -> i32 {
.entry:
    switch.i8 %x, default .d [ 256 -> .d ]
.d:
    ret 0
}
