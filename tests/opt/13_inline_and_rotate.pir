// OPT_LEVEL: -O2
// EXPECT_RET: 0
// EXPECT_LIR_FORBIDS: call @rotr32
// EXPECT_LIR_FORBIDS: call @clamp_i32
// Regression test: PIR LIR small-function inliner (single-block & multi-block
// with multiple returns) + post-inline constant propagation & shift folding.

target "aarch64-apple-darwin"

func @rotr32(%x: i32, %n: i32) -> i32 {
.entry:
    %r = shr.i32 %x, %n [mask]
    %m = sub.i32 32, %n
    %l = shl.i32 %x, %m [mask]
    %o = or.i32 %r, %l
    ret %o
}

func @clamp_i32(%v: i32, %lo: i32, %hi: i32) -> i32 {
.entry:
    %lt = cmp_lt_s.i32 %v, %lo
    br %lt, .ret_lo, .check_hi

.ret_lo:
    ret %lo

.check_hi:
    %gt = cmp_gt_s.i32 %v, %hi
    br %gt, .ret_hi, .ret_v

.ret_hi:
    ret %hi

.ret_v:
    ret %v
}

export func @main() -> i32 cc "c" {
.entry:
    %r1 = call @rotr32(305419896, 8)
    %r2 = call @rotr32(%r1, 24)
    %ok1 = cmp_eq.i32 %r2, 305419896

    %c1 = call @clamp_i32(-5, 0, 10)
    %c2 = call @clamp_i32(42, 0, 10)
    %c3 = call @clamp_i32(7, 0, 10)
    %sum = add.i32 %c1, %c2
    %sum2 = add.i32 %sum, %c3
    %ok2 = cmp_eq.i32 %sum2, 17

    %ok = and.i8 %ok1, %ok2
    %res = select.i32 %ok, 0, 1
    ret %res
}
