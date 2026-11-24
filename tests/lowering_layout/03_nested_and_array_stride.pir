// MODE: HIR
// EXPECT_LAYOUT: @Padded8 size=8 align=4 offsets=0,4
// EXPECT_LAYOUT: @Nested size=16 align=4 offsets=0,4,12
// EXPECT_LAYOUT: @Grid2D size=24 align=2 stride=8
// EXPECT_LAYOUT: @PaddedArr size=40 align=4 stride=8
target "x86_64-unknown-linux-gnu"

type @Padded8 = { i32, i8 }
type @Nested = { i8, @Padded8, i8 }
type @Grid2D = [3 x [4 x i16]]
type @PaddedArr = [5 x @Padded8]

export func @probe(%idx: i64) -> ptr {
.entry:
    %n = alloca @Nested
    %n2 = field_ptr @Nested, %n, 2
    store.i8 1, %n2, align 1

    %g = alloca @Grid2D
    %gr = elem_ptr @Grid2D, %g, %idx

    %pa = alloca @PaddedArr
    %pe = elem_ptr @PaddedArr, %pa, %idx
    ret %pe
}
