// MODE: LIR
// EXPECT_RET: 29
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // INT_MIN / -1 with [wrap] must NOT trap; evaluates to INT_MIN, remainder 0
    %min32 = const.i32 -2147483648
    %q = div_s.i32 %min32, -1 [wrap]
    %r = rem_s.i32 %min32, -1 [wrap]
    %k1 = cmp_eq.i32 %q, -2147483648
    %k2 = cmp_eq.i32 %r, 0

    // Signed vs unsigned comparisons on -1 vs 1
    %s_lt = cmp_lt_s.i32 -1, 1
    %u_gt = cmp_gt_u.i32 -1, 1
    %u_le = cmp_le_u.i32 0, -1
    %k3 = and.i8 %s_lt, %u_gt
    %k4 = and.i8 %k3, %u_le
    %k5 = and.i8 %k1, %k2
    %ok = and.i8 %k4, %k5
    br %ok, .done, .fail

.done:
    ret 29

.fail:
    trap
}
