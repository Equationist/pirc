// MODE: LIR
// EXPECT_RET: 75
target "aarch64-apple-darwin"

// High GP register pressure across a call:
// 18 simultaneously live i64 values across @clobber_gp force spills and callee-saved x19..x28 preservation.
func @clobber_gp(%x: i64) -> i64 [nounwind] {
.entry:
    %t0 = add.i64 %x, 11
    %t1 = mul.i64 %t0, 3
    %t2 = xor.i64 %t1, 0x55
    %t3 = add.i64 %t2, %t0
    %t4 = sub.i64 %t3, %t1
    %t5 = xor.i64 %t4, %t2
    %t6 = add.i64 %t5, %t3
    %t7 = sub.i64 %t6, %t4
    %t8 = mul.i64 %x, 3
    %t9 = xor.i64 %t8, 7
    %keep = xor.i64 %t7, %t7
    %res = add.i64 %t9, %keep
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %v0  = const.i64 1
    %v1  = add.i64 %v0, 1
    %v2  = add.i64 %v1, 1
    %v3  = add.i64 %v2, 1
    %v4  = add.i64 %v3, 1
    %v5  = add.i64 %v4, 1
    %v6  = add.i64 %v5, 1
    %v7  = add.i64 %v6, 1
    %v8  = add.i64 %v7, 1
    %v9  = add.i64 %v8, 1
    %v10 = add.i64 %v9, 1
    %v11 = add.i64 %v10, 1
    %v12 = add.i64 %v11, 1
    %v13 = add.i64 %v12, 1
    %v14 = add.i64 %v13, 1
    %v15 = add.i64 %v14, 1
    %v16 = add.i64 %v15, 1
    %v17 = add.i64 %v16, 1

    // All 18 values (%v0..%v17) remain live across this call:
    // clobber_gp(1) = (1 * 3) ^ 7 = 4
    %c = call @clobber_gp(%v0)

    %s0  = add.i64 %v0, %v1
    %s1  = add.i64 %v2, %v3
    %s2  = add.i64 %v4, %v5
    %s3  = add.i64 %v6, %v7
    %s4  = add.i64 %v8, %v9
    %s5  = add.i64 %v10, %v11
    %s6  = add.i64 %v12, %v13
    %s7  = add.i64 %v14, %v15
    %s8  = add.i64 %v16, %v17

    %p0 = add.i64 %s0, %s1
    %p1 = add.i64 %s2, %s3
    %p2 = add.i64 %s4, %s5
    %p3 = add.i64 %s6, %s7
    %q0 = add.i64 %p0, %p1
    %q1 = add.i64 %p2, %p3
    %q2 = add.i64 %q0, %q1
    %tot = add.i64 %q2, %s8

    // 1..18 = 171; 171 - 100 + 4 = 75
    %sub = sub.i64 %tot, 100
    %ans = add.i64 %sub, %c
    %res = trunc.i32 %ans
    ret %res
}
