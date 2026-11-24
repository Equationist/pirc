// MODE: LIR
// EXPECT_ERR: LIR vector width 256 exceeds target max_simd_bits 128
target "x86_64-unknown-linux-gnu"

func @bad(%a: v8f32, %b: v8f32) -> v8f32 {
.entry:
    %r = fadd.v8f32 %a, %b
    ret %r
}
