// MODE: HIR
// EXPECT_RET: 54
target "x86_64-unknown-linux-gnu"

type @Inner = { i8, i64, i8, i32 }
type @Outer = { i8, @Inner, i16 }

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %o = alloca @Outer
    %o_tag = field_ptr @Outer, %o, 0
    %o_in  = field_ptr @Outer, %o, 1
    %o_end = field_ptr @Outer, %o, 2

    store.i8 7, %o_tag, align 1
    store.i16 11, %o_end, align 2

    %in_a = field_ptr @Inner, %o_in, 0
    %in_b = field_ptr @Inner, %o_in, 1
    %in_c = field_ptr @Inner, %o_in, 2
    %in_d = field_ptr @Inner, %o_in, 3

    store.i8 3, %in_a, align 1
    store.i64 20, %in_b, align 8
    store.i8 5, %in_c, align 1
    store.i32 8, %in_d, align 4

    // Reload every field to ensure zero overlap across padded regions
    %v_tag8 = load.i8 %o_tag, align 1
    %v_end16 = load.i16 %o_end, align 2
    %v_a8 = load.i8 %in_a, align 1
    %v_b64 = load.i64 %in_b, align 8
    %v_c8 = load.i8 %in_c, align 1
    %v_d32 = load.i32 %in_d, align 4

    %v_tag = zext.i32 %v_tag8
    %v_end = zext.i32 %v_end16
    %v_a = zext.i32 %v_a8
    %v_b = trunc.i32 %v_b64
    %v_c = zext.i32 %v_c8

    // 7 + 11 + 3 + 20 + 5 + 8 = 54
    %s1 = add.i32 %v_tag, %v_end
    %s2 = add.i32 %v_a, %v_b
    %s3 = add.i32 %v_c, %v_d32
    %s4 = add.i32 %s1, %s2
    %res = add.i32 %s4, %s3
    ret %res
}
