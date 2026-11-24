// EXPECT_OUT: 1 0 0 1
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%d %d %d %d\n", 0 }

func @is_even(%n: i32) -> i32 {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .yes, .rec
.yes:
    ret 1
.rec:
    %m = sub.i32 %n, 1
    %r = call @is_odd(%m)
    ret %r
}

func @is_odd(%n: i32) -> i32 {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .no, .rec
.no:
    ret 0
.rec:
    %m = sub.i32 %n, 1
    %r = call @is_even(%m)
    ret %r
}

export func @main() -> i32 {
.entry:
    %a = call @is_even(12)
    %b = call @is_odd(12)
    %c = call @is_even(13)
    %d = call @is_odd(13)
    %f = addr @fmt
    call @printf(%f, %a, %b, %c, %d)
    ret 0
}
