target "x86_64-unknown-linux-gnu"

type @Buf = [256 x i8]
type @Inner = packed { i8, i16 }
type @Val = union { i32, f32, @Inner }
type @Outer = { @Buf, @Val }

export func @touch_nested(%i: i64) -> i8 {
.entry:
    %o   = alloca @Outer, align 4
    %bp  = field_ptr @Outer, %o, 0
    %ep  = elem_ptr i8, %bp, %i
    store.i8 7, %ep, align 1
    %vp  = field_ptr @Outer, %o, 1
    %ip  = field_ptr @Val, %vp, 2
    %f0  = field_ptr @Inner, %ip, 0
    %v   = load.i8 %f0, align 1
    ret %v
}
