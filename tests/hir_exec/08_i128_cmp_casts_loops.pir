// MODE: HIR
// EXPECT_RET: 91
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Signed vs unsigned comparisons: -1 vs 1, and high-word-tie comparisons
    %neg1 = const.i128 -1
    %pos1 = const.i128 1
    %c1 = cmp_lt_s.i128 %neg1, %pos1
    %c2 = cmp_gt_u.i128 %neg1, %pos1

    // Same high word (1), different low word (5 vs 9)
    %hi1 = shl.i128 %pos1, 64 [mask]
    %v_a = add.i128 %hi1, 5
    %v_b = add.i128 %hi1, 9
    %c3 = cmp_lt_u.i128 %v_a, %v_b
    %c4 = cmp_gt_s.i128 %v_b, %v_a
    %sel = select.i128 %c3, %v_b, %v_a
    %c5 = cmp_eq.i128 %sel, %v_b

    %m1 = and.i8 %c1, %c2
    %m2 = and.i8 %c3, %c4
    %m3 = and.i8 %m1, %m2
    %ok = and.i8 %m3, %c5
    br %ok, .loop_start, .fail

.loop_start:
    // Loop with i128 block arguments: sum_{k=1..6} (2^64 + k) = 6*2^64 + 21
    jmp .loop(1, 0)

.loop(%k: i128, %acc: i128):
    %done = cmp_gt_u.i128 %k, 6
    br %done, .finish(%acc), .body

.body:
    %term = add.i128 %hi1, %k
    %acc1 = add.i128 %acc, %term
    %k1 = add.i128 %k, 1
    jmp .loop(%k1, %acc1)

.finish(%tot: i128):
    %tot_lo = trunc.i32 %tot
    %tot_hi128 = shr.i128 %tot, 64 [mask]
    %tot_hi = trunc.i32 %tot_hi128
    %h_ok = cmp_eq.i32 %tot_hi, 6
    br %h_ok, .ret_ok(%tot_lo), .fail

.ret_ok(%lo: i32):
    // 21 + 70 = 91
    %res = add.i32 %lo, 70
    ret %res

.fail:
    trap
}
