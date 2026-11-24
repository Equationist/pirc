// OPT_LEVEL: -O2
// EXPECT_RET: 0
// EXPECT_LIR_CONTAINS: select.f32
// EXPECT_LIR_FORBIDS: alloca
// Regression test: zero-offset ptr_add mem2reg, unary const cast folding,
// cmp_ne(zext(cmp), 0) simplification, and diamond if-conversion into select.

target "aarch64-apple-darwin"

export func @clamp_scalar(%x: f32, %lo: f32, %hi: f32) -> f32 cc "c" {
.entry:
    %lv = alloca f32, align 4
    %p0 = ptr_add.i64 %lv, 0
    store.f32 %x, %p0, align 4
    %p1 = ptr_add.i64 %lv, 0
    %v0 = load.f32 %p1, align 4
    %gt = fcmp_ogt.f32 %v0, %hi
    %gz = zext.i32 %gt
    %gb = cmp_ne.i32 %gz, 0
    br %gb, .hi_set, .after_hi

.hi_set:
    %p2 = ptr_add.i64 %lv, 0
    store.f32 %hi, %p2, align 4
    jmp .after_hi

.after_hi:
    %p3 = ptr_add.i64 %lv, 0
    %v1 = load.f32 %p3, align 4
    %lt = fcmp_olt.f32 %v1, %lo
    %lz = zext.i32 %lt
    %lb = cmp_ne.i32 %lz, 0
    br %lb, .lo_set, .done

.lo_set:
    %p4 = ptr_add.i64 %lv, 0
    store.f32 %lo, %p4, align 4
    jmp .done

.done:
    %p5 = ptr_add.i64 %lv, 0
    %vr = load.f32 %p5, align 4
    ret %vr
}

export func @main() -> i32 cc "c" {
.entry:
    %a = call @clamp_scalar(5.0, -2.0, 2.0)
    %b = call @clamp_scalar(-7.0, -2.0, 2.0)
    %c = call @clamp_scalar(1.5, -2.0, 2.0)
    %c1 = fcmp_oeq.f32 %a, 2.0
    %c2 = fcmp_oeq.f32 %b, -2.0
    %c3 = fcmp_oeq.f32 %c, 1.5
    %t1 = and.i8 %c1, %c2
    %ok = and.i8 %t1, %c3
    %res = select.i32 %ok, 0, 1
    ret %res
}
