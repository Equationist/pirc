// MODE: LIR
// REQUIRES_SIMD: 128
// EXPECT_RET: 58
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %va = splat.v4i32 10
    %va1 = insert.v4i32 %va, 5, 0
    %va2 = insert.v4i32 %va1, 20, 2
    // va2 = [5, 10, 20, 10]
    %vb = splat.v4i32 3
    // sum = [8, 13, 23, 13]
    %sum = add.v4i32 %va2, %vb
    // mask = (sum < 15) = [-1, -1, 0, -1]
    %lim = splat.v4i32 15
    %mask = cmp_lt_s.v4i32 %sum, %lim
    // chosen = select(mask, sum, lim) = [8, 13, 15, 13] -> total = 49
    %ch = select.v4i32 %mask, %sum, %lim

    %e0 = extract.v4i32 %ch, 0
    %e1 = extract.v4i32 %ch, 1
    %e2 = extract.v4i32 %ch, 2
    %e3 = extract.v4i32 %ch, 3
    %t1 = add.i32 %e0, %e1
    %t2 = add.i32 %e2, %e3
    %t3 = add.i32 %t1, %t2

    // v16i8 lane-wise add + extract
    %b1 = splat.v16i8 4
    %b2 = splat.v16i8 5
    %bsum = add.v16i8 %b1, %b2
    %lane7 = extract.v16i8 %bsum, 7
    %l7_i32 = zext.i32 %lane7
    // 49 + 9 = 58
    %res = add.i32 %t3, %l7_i32
    ret %res
}
