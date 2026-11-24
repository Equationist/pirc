// MODE: HIR
// EXPECT_LAYOUT: @MixedA size=24 align=8 offsets=0,8,16,20
// EXPECT_LAYOUT: @MixedB size=12 align=4 offsets=0,2,4,8
target "x86_64-unknown-linux-gnu"

type @MixedA = { i8, i64, i8, i32 }
type @MixedB = { i8, i16, i32, i8 }

export func @probe_a() -> ptr {
.entry:
    %a = alloca @MixedA
    %a0 = field_ptr @MixedA, %a, 0
    %a1 = field_ptr @MixedA, %a, 1
    %a2 = field_ptr @MixedA, %a, 2
    %a3 = field_ptr @MixedA, %a, 3
    store.i8 1, %a0, align 1
    store.i64 2, %a1, align 8
    store.i8 3, %a2, align 1
    store.i32 4, %a3, align 4
    ret %a3
}

export func @probe_b() -> ptr {
.entry:
    %b = alloca @MixedB
    %b0 = field_ptr @MixedB, %b, 0
    %b1 = field_ptr @MixedB, %b, 1
    %b2 = field_ptr @MixedB, %b, 2
    %b3 = field_ptr @MixedB, %b, 3
    store.i8 1, %b0, align 1
    store.i16 2, %b1, align 2
    store.i32 3, %b2, align 4
    store.i8 4, %b3, align 1
    ret %b3
}
