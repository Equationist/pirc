// MODE: LIR
// EXPECT_RET: 42
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %a = const.i32 100
    %b = const.i32 23
    %s = add.i32 %a, %b [unsafe_s, unsafe_u]
    %d = sub.i32 %s, 13 [wrap]
    %m = mul.i32 %d, 2 [unsafe_s]
    %q = div_s.i32 %m, 5 [trap]
    %r = rem_s.i32 %m, 5 [trap]
    %neg = sub.i32 0, 17
    %q2 = div_s.i32 %neg, 5 [trap]
    %r2 = rem_s.i32 %neg, 5 [trap]
    %u = const.i32 -1
    %uq = div_u.i32 %u, 65536 [trap]
    %ur = rem_u.i32 %uq, 100 [trap]
    %w1 = const.i32 2147483647
    %w2 = add.i32 %w1, 1 [wrap]
    %w_ok = cmp_eq.i32 %w2, -2147483648
    br %w_ok, .chk1, .fail

.chk1:
    // m=220, q=44, r=0, q2=-3, r2=-2, uq=65535, ur=35
    %t1 = add.i32 %q, %r
    %t2 = add.i32 %t1, %q2
    %t3 = add.i32 %t2, %r2
    // t3 = 44 + 0 - 3 - 2 = 39
    %c1 = cmp_eq.i32 %t3, 39
    %c2 = cmp_eq.i32 %ur, 35
    %ok = and.i8 %c1, %c2
    br %ok, .done(%t3), .fail

.done(%v: i32):
    %res = add.i32 %v, 3
    ret %res

.fail:
    trap
}
