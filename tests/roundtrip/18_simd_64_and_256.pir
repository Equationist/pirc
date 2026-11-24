target "x86_64-unknown-linux-gnu"

export func @simd_64_256(%p: ptr, %q: ptr) -> f64 {
.entry:
    %a64  = load.v8i8 %p, align 8
    %b64  = add.v8i8 %a64, %a64
    store.v8i8 %b64, %q, align 8

    %w16  = load.v4i16 %p, align 8
    %w32  = zext.v4i32 %w16
    %w16b = trunc.v4i16 %w32
    store.v4i16 %w16b, %q, align 8

    %v2f  = load.v2f32 %p, align 8
    %v2d  = fpext.v2f64 %v2f
    %v2f2 = fptrunc.v2f32 %v2d
    %v2i  = fptosi.v2i32 %v2f2 [sat]
    %v2f3 = sitofp.v2f32 %v2i
    %bc64 = bitcast.v2i32 %v2f3
    store.v2i32 %bc64, %q, align 8

    %a256 = load.v32i8 %p, align 32
    %b256 = xor.v32i8 %a256, %a256
    store.v32i8 %b256, %q, align 32

    %h256 = load.v16i16 %p, align 32
    store.v16i16 %h256, %q, align 32

    %i256 = load.v8i32 %p, align 32
    %f256 = sitofp.v8f32 %i256
    store.v8f32 %f256, %q, align 32

    %l256 = load.v4i64 %p, align 32
    %d256 = uitofp.v4f64 %l256
    %d0   = extract.v4f64 %d256, 3
    ret %d0
}
