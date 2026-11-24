// EXPECT_OUT: 0.50 1.00 9.00 1.00
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%.2f %.2f %.2f %.2f\n", 0 }

export func @main() -> i32 {
.entry:
    %buf = alloca.ptr 16, align 16
    %p0  = ptr_add.i64 %buf, 0 [inbounds]
    %p1  = ptr_add.i64 %buf, 4 [inbounds]
    %p2  = ptr_add.i64 %buf, 8 [inbounds]
    %p3  = ptr_add.i64 %buf, 12 [inbounds]
    store.f32 0.25, %p0, align 4
    store.f32 0.75, %p1, align 4
    store.f32 1.5,  %p2, align 4
    store.f32 2.0,  %p3, align 4

    %v    = load.v4f32 %buf, align 16
    %vg   = splat.v4f32 2.0
    %vmax = splat.v4f32 1.0
    %sc   = fmul.v4f32 %v, %vg [fast]
    %mask = fcmp_ogt.v4f32 %sc, %vmax
    %cl   = select.v4f32 %mask, %vmax, %sc
    %ins  = insert.v4f32 %cl, 9.0, 2

    %e0 = extract.v4f32 %ins, 0
    %e1 = extract.v4f32 %ins, 1
    %e2 = extract.v4f32 %ins, 2
    %e3 = extract.v4f32 %ins, 3
    %f  = addr @fmt
    call @printf(%f, %e0, %e1, %e2, %e3)
    ret 0
}
