target "aarch64-apple-darwin"

func @f() -> i32 cc "c" {
.entry:
    ret 0
.dead:
    %y = add.i32 %x, 1 [wrap]
    %x = const.i32 5
    ret %y
}
