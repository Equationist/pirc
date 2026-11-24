// MODE: HIR
// TEST_SIMD_CAPS: 256, 128, 0
// EXPECT_RET: 73
target "x86_64-unknown-linux-gnu"

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. v8i32 arithmetic + shifts + signed comparison + select
    %a = splat.v8i32 4
    %a1 = insert.v8i32 %a, -8, 0
    %a2 = insert.v8i32 %a1, 12, 5
    // a2 = [-8, 4, 4, 4, 4, 12, 4, 4]
    %sh = splat.v8i32 1
    // sar by 1 -> [-4, 2, 2, 2, 2, 6, 2, 2]
    %s = sar.v8i32 %a2, %sh [mask]
    %zero = splat.v8i32 0
    %neg_mask = cmp_lt_s.v8i32 %s, %zero
    %ten = splat.v8i32 10
    // replace negative lane 0 with 10 -> [10, 2, 2, 2, 2, 6, 2, 2] -> sum = 28
    %cl = select.v8i32 %neg_mask, %ten, %s

    %e0 = extract.v8i32 %cl, 0
    %e1 = extract.v8i32 %cl, 1
    %e5 = extract.v8i32 %cl, 5
    %e7 = extract.v8i32 %cl, 7
    // 10 + 2 + 6 + 2 = 20
    %p1 = add.i32 %e0, %e1
    %p2 = add.i32 %e5, %e7
    %s_i32 = add.i32 %p1, %p2

    // 2. v4i64 bitwise xor + add
    %q1 = splat.v4i64 15
    %q2 = splat.v4i64 6
    %qx = xor.v4i64 %q1, %q2
    // 15 ^ 6 = 9 in every lane; extract lane 3
    %q3 = extract.v4i64 %qx, 3
    %q3_32 = trunc.i32 %q3

    // 3. v4f32 <-> v4i32 bitcast check (1.0f == 0x3F800000)
    %vf = splat.v4f32 1.0
    %vi = bitcast.v4i32 %vf
    %bits0 = extract.v4i32 %vi, 0
    %bc_ok = cmp_eq.i32 %bits0, 0x3F800000
    br %bc_ok, .done(%s_i32, %q3_32), .fail

.done(%x: i32, %y: i32):
    // 20 + 9 + 44 = 73
    %t = add.i32 %x, %y
    %res = add.i32 %t, 44
    ret %res

.fail:
    trap
}
