target "aarch64-apple-darwin"

file 0 "test.c"

func @bad_loc() -> i32 {
.entry:
    loc 0, 1, 1 garbage
    ret 0
}
