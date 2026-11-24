target "aarch64-apple-darwin"

type @Vec2 = { f32, f32 }

export func @vec_sum(%out: retbuf(@Vec2), %a: byval(@Vec2), %b: byval(@Vec2)) -> void cc "c" section "__TEXT,__pir" nounwind readonly {
.entry:
    %axp = field_ptr @Vec2, %a, 0
    %ayp = field_ptr @Vec2, %a, 1
    %bxp = field_ptr @Vec2, %b, 0
    %byp = field_ptr @Vec2, %b, 1
    %ax  = load.f32 %axp, align 4
    %ay  = load.f32 %ayp, align 4
    %bx  = load.f32 %bxp, align 4
    %by  = load.f32 %byp, align 4
    %rx  = fadd.f32 %ax, %bx
    %ry  = fadd.f32 %ay, %by
    %oxp = field_ptr @Vec2, %out, 0
    %oyp = field_ptr @Vec2, %out, 1
    store.f32 %rx, %oxp, align 4
    store.f32 %ry, %oyp, align 4
    ret
}
