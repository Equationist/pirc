// MODE: HIR
// EXPECT_LAYOUT: @WithI128 size=32 align=16 offsets=0,16
// EXPECT_LAYOUT: @WithV128 size=32 align=16 offsets=0,16
// EXPECT_LAYOUT: @WithV256 size=64 align=32 offsets=0,32
target "x86_64-unknown-linux-gnu"

type @WithI128 = { i8, i128 }
type @WithV128 = { i8, v4f32 }
type @WithV256 = { i8, v8f32 }

export func @probe() -> ptr {
.entry:
    %a = alloca @WithI128
    %a1 = field_ptr @WithI128, %a, 1

    %b = alloca @WithV128
    %b1 = field_ptr @WithV128, %b, 1

    %c = alloca @WithV256
    %c1 = field_ptr @WithV256, %c, 1
    ret %c1
}
