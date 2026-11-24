// MODE: LIR
// EXPECT_RET: 89
target "x86_64-unknown-linux-gnu"

func @fib_rec(%n: i32) -> i32 [nounwind] {
.entry:
    %base = cmp_le_s.i32 %n, 1
    br %base, .ret_n, .recurse

.ret_n:
    ret %n

.recurse:
    %n1 = sub.i32 %n, 1
    %n2 = sub.i32 %n, 2
    %a = call @fib_rec(%n1)
    %b = call @fib_rec(%n2)
    %s = add.i32 %a, %b
    ret %s
}

func @is_even(%n: i32) -> i8 [nounwind] {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .yes, .step

.yes:
    ret 1

.step:
    %m = sub.i32 %n, 1
    %r = call @is_odd(%m)
    ret %r
}

func @is_odd(%n: i32) -> i8 [nounwind] {
.entry:
    %z = cmp_eq.i32 %n, 0
    br %z, .no, .step

.no:
    ret 0

.step:
    %m = sub.i32 %n, 1
    %r = call @is_even(%m)
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // fib_rec(11) = 89; is_even(14) = 1; is_odd(13) = 1; is_even(13) = 0
    %f = call @fib_rec(11)
    %e1 = call @is_even(14)
    %o1 = call @is_odd(13)
    %e2 = call @is_even(13)
    %ne2 = xor.i8 %e2, 1
    %ok1 = and.i8 %e1, %o1
    %ok = and.i8 %ok1, %ne2
    br %ok, .done, .fail

.done:
    ret %f

.fail:
    trap
}
