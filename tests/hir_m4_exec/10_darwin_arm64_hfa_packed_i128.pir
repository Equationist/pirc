// MODE: HIR
// EXPECT_RET: 63
target "aarch64-apple-darwin"

// Exercises Darwin ARM64 C ABI execution end-to-end:
//  - 4-element HFA passed in s0..s3
//  - 16B mixed int/float struct passed in GP pair x0, x1
//  - 9B packed struct passed in GP pair x0, x1 (without OOB read on byte 9..15)
//  - (i32, i128) passed in w0, x1:x2 without odd-register padding
type @Hfa4    = { f32, f32, f32, f32 }
type @Mixed16 = { i64, f64 }
type @Packed9 = packed { i8, i64 }

export func @sum_hfa4(%h: byval(@Hfa4)) -> f32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4, %h, 0
    %p1 = field_ptr @Hfa4, %h, 1
    %p2 = field_ptr @Hfa4, %h, 2
    %p3 = field_ptr @Hfa4, %h, 3
    %v0 = load.f32 %p0, align 4
    %v1 = load.f32 %p1, align 4
    %v2 = load.f32 %p2, align 4
    %v3 = load.f32 %p3, align 4
    %s1 = fadd.f32 %v0, %v1
    %s2 = fadd.f32 %v2, %v3
    %res = fadd.f32 %s1, %s2
    ret %res
}

export func @sum_mixed16(%m: byval(@Mixed16)) -> i64 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Mixed16, %m, 0
    %p1 = field_ptr @Mixed16, %m, 1
    %a = load.i64 %p0, align 8
    %b = load.f64 %p1, align 8
    %bi = fptosi.i64 %b [sat]
    %res = add.i64 %a, %bi
    ret %res
}

export func @sum_packed9(%p: byval(@Packed9)) -> i64 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Packed9, %p, 0
    %p1 = field_ptr @Packed9, %p, 1
    %a = load.i8 %p0, align 1
    %b = load.i64 %p1, align 1
    // Mutate local copy to verify byval copy isolation
    store.i8 99, %p0, align 1
    store.i64 9999, %p1, align 1
    %a64 = zext.i64 %a
    %res = add.i64 %a64, %b
    ret %res
}

export func @darwin_i32_then_i128(%tag: i32, %big: i128) -> i64 cc "c" [nounwind] {
.entry:
    %lo = trunc.i64 %big
    %shifted = shr.i128 %big, 64
    %hi = trunc.i64 %shifted
    %tag64 = sext.i64 %tag
    %s1 = add.i64 %tag64, %lo
    %res = add.i64 %s1, %hi
    ret %res
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // 1. Hfa4: 1.0 + 2.0 + 3.0 + 4.0 = 10
    %h = alloca @Hfa4
    %h0 = field_ptr @Hfa4, %h, 0
    %h1 = field_ptr @Hfa4, %h, 1
    %h2 = field_ptr @Hfa4, %h, 2
    %h3 = field_ptr @Hfa4, %h, 3
    store.f32 1.0, %h0, align 4
    store.f32 2.0, %h1, align 4
    store.f32 3.0, %h2, align 4
    store.f32 4.0, %h3, align 4
    %hf = call @sum_hfa4(%h)
    %r1 = fptosi.i32 %hf [sat]

    // 2. Mixed16: 12 + 3.0 = 15
    %m = alloca @Mixed16
    %m0 = field_ptr @Mixed16, %m, 0
    %m1 = field_ptr @Mixed16, %m, 1
    store.i64 12, %m0, align 8
    store.f64 3.0, %m1, align 8
    %r2_64 = call @sum_mixed16(%m)
    %r2 = trunc.i32 %r2_64

    // 3. Packed9: 4 + 11 = 15 (and original stays intact)
    %p = alloca @Packed9
    %p0 = field_ptr @Packed9, %p, 0
    %p1 = field_ptr @Packed9, %p, 1
    store.i8 4, %p0, align 1
    store.i64 11, %p1, align 1
    %r3_64 = call @sum_packed9(%p)
    %r3 = trunc.i32 %r3_64
    %orig_p0 = load.i8 %p0, align 1
    %orig_ok = cmp_eq.i8 %orig_p0, 4
    br %orig_ok, .step_i128, .fail

.step_i128:
    // 4. Darwin unpadded (i32, i128): tag=5, big=(1<<64)+17 -> 5 + 17 + 1 = 23
    %one = const.i128 1
    %hi_part = shl.i128 %one, 64
    %big = add.i128 %hi_part, 17
    %r4_64 = call @darwin_i32_then_i128(5, %big)
    %r4 = trunc.i32 %r4_64

    // 10 + 15 + 15 + 23 = 63
    %s1 = add.i32 %r1, %r2
    %s2 = add.i32 %r3, %r4
    %res = add.i32 %s1, %s2
    ret %res

.fail:
    trap
}
