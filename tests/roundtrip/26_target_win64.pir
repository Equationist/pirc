target "x86_64-pc-windows-msvc"

export func @even(%n: i32) -> i8 cc "internal" {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .yes, .rec

.yes:
    ret 1

.rec:
    %m = sub.i32 %n, 1
    %r = call @odd(%m)
    ret %r
}

export func @odd(%n: i32) -> i8 {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .no, .rec

.no:
    ret 0

.rec:
    %m = sub.i32 %n, 1
    %r = call @even(%m)
    ret %r
}
