// MODE: HIR
// EXPECT_RET: 48
target "x86_64-unknown-linux-gnu"

type @Vec4 = { i32, i32, i32, i32 }
type @Buf16 = [16 x i8]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Small constant-length memcpy (16 bytes -> unrollable path)
    %src = alloca @Vec4
    %dst = alloca @Vec4
    %s0 = field_ptr @Vec4, %src, 0
    %s1 = field_ptr @Vec4, %src, 1
    %s2 = field_ptr @Vec4, %src, 2
    %s3 = field_ptr @Vec4, %src, 3
    store.i32 5, %s0, align 4
    store.i32 7, %s1, align 4
    store.i32 9, %s2, align 4
    store.i32 11, %s3, align 4

    memcpy %dst, %src, 16, align 4

    %d0 = field_ptr @Vec4, %dst, 0
    %d1 = field_ptr @Vec4, %dst, 1
    %d2 = field_ptr @Vec4, %dst, 2
    %d3 = field_ptr @Vec4, %dst, 3
    %v0 = load.i32 %d0, align 4
    %v1 = load.i32 %d1, align 4
    %v2 = load.i32 %d2, align 4
    %v3 = load.i32 %d3, align 4
    // 5 + 7 + 9 + 11 = 32
    %t1 = add.i32 %v0, %v1
    %t2 = add.i32 %v2, %v3
    %vec_sum = add.i32 %t1, %t2

    // Small constant-length memset (16 bytes of value 2)
    %buf = alloca @Buf16
    memset %buf, 2, 16, align 1
    jmp .sum_buf(0, 0)

.sum_buf(%k: i64, %bacc: i32):
    %done = cmp_ge_u.i64 %k, 8
    br %done, .finish(%bacc), .sum_step

.sum_step:
    %bp = elem_ptr @Buf16, %buf, %k
    %bv8 = load.i8 %bp, align 1
    %bv = zext.i32 %bv8
    %bacc1 = add.i32 %bacc, %bv
    %k1 = add.i64 %k, 1
    jmp .sum_buf(%k1, %bacc1)

.finish(%buf_sum: i32):
    // 32 + (8 * 2) = 48
    %res = add.i32 %vec_sum, %buf_sum
    ret %res
}
