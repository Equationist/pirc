// MODE: HIR
// EXPECT_RET: 42
// R12: SysV x86-64 retbuf for packed { i64, f32, pad 1 } (size 13)
target "x86_64-unknown-linux-gnu"

type @S = packed { i64, f32, pad 1 }

func @make(%out: retbuf(@S)) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @S, %out, 0
    store.i64 35, %p0, align 1
    %p1 = field_ptr @S, %out, 1
    store.f32 7.0, %p1, align 1
    ret
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %s = alloca @S
    call @make(%s)
    %p0 = field_ptr @S, %s, 0
    %p1 = field_ptr @S, %s, 1
    %a = load.i64 %p0, align 1
    %b = load.f32 %p1, align 1
    %a32 = trunc.i32 %a
    %b32 = fptosi.i32 %b [sat]
    %sum = add.i32 %a32, %b32
    ret %sum
}
