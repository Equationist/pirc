target "x86_64-unknown-linux-gnu"

export func @simd_clamp(%dst: ptr, %src: ptr, %gain: f32) -> f32 {
.entry:
    %vg   = splat.v4f32 %gain
    %vmax = splat.v4f32 1.0
    %v    = load.v4f32 %src, align 16
    %sc   = fmul.v4f32 %v, %vg [fast]
    %mask = fcmp_ogt.v4f32 %sc, %vmax
    %cl   = select.v4f32 %mask, %vmax, %sc
    %sh   = shuffle.v4f32 %cl, %vg [0, 4, 1, 5]
    %ins  = insert.v4f32 %sh, %gain, 2
    store.v4f32 %ins, %dst, align 16
    %e0   = extract.v4f32 %ins, 0
    ret %e0
}
