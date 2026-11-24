// MODE: HIR
// EXPECT_RET: 44
target "x86_64-unknown-linux-gnu"

// Exercises i128 div_s/div_u/rem_s/rem_u lowering to __divti3/__udivti3/__modti3/__umodti3
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Build N = (10 * 2^64) + 37
    %ten = const.i128 10
    %hi = shl.i128 %ten, 64 [mask]
    %n = add.i128 %hi, 37

    // Unsigned divide by 2^64 -> q = 10, r = 37
    %one = const.i128 1
    %d = shl.i128 %one, 64 [mask]
    %qu = div_u.i128 %n, %d [trap]
    %ru = rem_u.i128 %n, %d [trap]
    %c1 = cmp_eq.i128 %qu, 10
    %c2 = cmp_eq.i128 %ru, 37

    // Signed negative division: (-N) / 2^64 == -10, rem == -37
    %neg_n = sub.i128 0, %n
    %qs = div_s.i128 %neg_n, %d [trap]
    %rs = rem_s.i128 %neg_n, %d [trap]
    %c3 = cmp_eq.i128 %qs, -10
    %c4 = cmp_eq.i128 %rs, -37

    %m1 = and.i8 %c1, %c2
    %m2 = and.i8 %c3, %c4
    %ok = and.i8 %m1, %m2
    br %ok, .done(%qu, %ru), .fail

.done(%q: i128, %r: i128):
    // 10 + 37 - 3 = 44
    %s = add.i128 %q, %r
    %s32 = trunc.i32 %s
    %res = sub.i32 %s32, 3
    ret %res

.fail:
    trap
}
