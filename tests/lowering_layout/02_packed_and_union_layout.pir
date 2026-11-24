// MODE: HIR
// EXPECT_LAYOUT: @Packed10 size=10 align=1 offsets=0,1,9
// EXPECT_LAYOUT: @Union8 size=8 align=8 offsets=0,0,0
// EXPECT_LAYOUT: @UnionTailPad size=12 align=4 offsets=0,0
target "x86_64-unknown-linux-gnu"

type @Packed10 = packed { i8, i64, i8 }
type @Union8 = union { [5 x i8], i32, i64 }
type @UnionTailPad = union { [9 x i8], i32 }

export func @probe() -> ptr {
.entry:
    %p = alloca @Packed10
    %p0 = field_ptr @Packed10, %p, 0
    %p1 = field_ptr @Packed10, %p, 1
    %p2 = field_ptr @Packed10, %p, 2
    store.i8 1, %p0, align 1
    store.i64 2, %p1, align 1
    store.i8 3, %p2, align 1

    %u1 = alloca @Union8
    %u1_2 = field_ptr @Union8, %u1, 2
    store.i64 99, %u1_2, align 8

    %u2 = alloca @UnionTailPad
    %u2_1 = field_ptr @UnionTailPad, %u2, 1
    store.i32 7, %u2_1, align 4
    ret %p2
}
