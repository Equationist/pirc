// MODE: HIR
// EXPECT_LAYOUT: @Node32 size=12 align=4 offsets=0,4,8
target "i686-unknown-linux-gnu"

type @Node32 = { i8, ptr, i8 }

export func @probe() -> ptr {
.entry:
    %n = alloca @Node32
    %n0 = field_ptr @Node32, %n, 0
    %n1 = field_ptr @Node32, %n, 1
    %n2 = field_ptr @Node32, %n, 2
    store.i8 1, %n0, align 1
    %nullp = const.ptr 0
    store.ptr %nullp, %n1, align 4
    store.i8 2, %n2, align 1
    ret %n2
}
