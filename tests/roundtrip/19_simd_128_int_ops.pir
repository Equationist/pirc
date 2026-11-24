target "x86_64-unknown-linux-gnu"

export func @simd_128_int(%p: ptr, %q: ptr, %k: i32) -> i64 {
.entry:
    %v8  = load.v16i8 %p, align 16
    %v8b = add.v16i8 %v8, %v8
    store.v16i8 %v8b, %q, align 16

    %v16 = load.v8i16 %p, align 16
    %v16b = sub.v8i16 %v16, %v16
    store.v8i16 %v16b, %q, align 16

    %vk  = splat.v4i32 %k
    %v32 = load.v4i32 %p, align 16
    %m   = cmp_lt_s.v4i32 %v32, %vk
    %sel = select.v4i32 %m, %vk, %v32
    %sh  = shuffle.v4i32 %sel, %vk [3, 2, 1, 0]
    %ins = insert.v4i32 %sh, %k, 1
    store.v4i32 %ins, %q, align 16

    %v64 = load.v2i64 %p, align 16
    %vd  = bitcast.v2f64 %v64
    %vd2 = fadd.v2f64 %vd, %vd
    %v64b = bitcast.v2i64 %vd2
    %e   = extract.v2i64 %v64b, 1
    ret %e
}
