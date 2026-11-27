target "x86_64-unknown-linux-gnu"
@g = global i32 = 1, align 4
export func @f() -> ptr {
.entry:
    %p = const.ptr @g - -8
    ret %p
}
