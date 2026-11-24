// CAPS: avx2
// EXPECT_OUT: 15.00 30
target "x86_64-unknown-linux-gnu"

extern func @printf(%fmt: ptr, ...) -> i32 cc "c"
@fmt = data align 1 { "%.2f %lld\n", 0 }

export func @main() -> i32 {
.entry:
    %a = splat.v8f32 2.5
    %b = splat.v8f32 6.0
    %c = fmul.v8f32 %a, %b
    %e = extract.v8f32 %c, 7

    %v1 = splat.v4i64 10
    %v2 = splat.v4i64 20
    %v3 = add.v4i64 %v1, %v2
    %l  = extract.v4i64 %v3, 3

    %f = addr @fmt
    call @printf(%f, %e, %l)
    ret 0
}
