// MODE: LIR
// EXPECT_RET: 31
target "x86_64-unknown-linux-gnu"

func @clamp_i32(%x: i32, %lo: i32, %hi: i32) -> i32 [nounwind] {
.entry:
    %lt = cmp_lt_s.i32 %x, %lo
    %v1 = select.i32 %lt, %lo, %x
    %gt = cmp_gt_s.i32 %v1, %hi
    %v2 = select.i32 %gt, %hi, %v1
    ret %v2
}

func @abs_i32(%x: i32) -> i32 [nounwind] {
.entry:
    %neg = sub.i32 0, %x
    %is_neg = cmp_lt_s.i32 %x, 0
    %r = select.i32 %is_neg, %neg, %x
    ret %r
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %c1 = call @clamp_i32(-50, 10, 20)
    %c2 = call @clamp_i32(15, 10, 20)
    %c3 = call @clamp_i32(99, 10, 20)
    %a1 = call @abs_i32(-14)
    // c1=10, c2=15, c3=20, a1=14 -> 10+15+20-14 = 31
    // Also exercise select.ptr
    %p1 = alloca.ptr 4, align 4
    %p2 = alloca.ptr 4, align 4
    store.i32 100, %p1, align 4
    store.i32 31, %p2, align 4
    %sum = add.i32 %c1, %c2
    %sum2 = add.i32 %sum, %c3
    %diff = sub.i32 %sum2, %a1
    %eq = cmp_eq.i32 %diff, 31
    %chosen = select.ptr %eq, %p2, %p1
    %res = load.i32 %chosen, align 4
    ret %res
}
