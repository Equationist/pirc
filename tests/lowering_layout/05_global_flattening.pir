// MODE: HIR
// EXPECT_LAYOUT: @Point size=8 align=4 offsets=0,4
// EXPECT_GLOBAL_LIR: @g_arr bytes=64 align=4 zeroinit
// EXPECT_GLOBAL_LIR: @g_point bytes=8 align=4 zeroinit
target "x86_64-unknown-linux-gnu"

type @Point = { i32, i32 }

@g_arr   = global [16 x i32] = zeroinit, align 4
@g_point = global @Point = zeroinit, align 4

export func @probe() -> i32 {
.entry:
    %gp = const.ptr @g_point
    %yp = field_ptr @Point, %gp, 1
    store.i32 12, %yp, align 4
    %v = load.i32 %yp, align 4
    ret %v
}
