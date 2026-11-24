target "x86_64-unknown-linux-gnu"

export func @answer() -> i32 cc "c" [nounwind] {
.entry:
    %v = const.i32 42
    ret %v
}
