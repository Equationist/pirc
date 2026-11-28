target "thumbv7em-none-eabihf"

// Bidirectional AAPCS32-VFP PIR side paired with tests/thumb2_exec/c_abi_check.c

type @Hfa4f  = { f32, f32, f32, f32 }
type @Hfa3d  = { f64, f64, f64 }
type @Small4 = { i8, i16 }
type @Mid12  = { i32, i32, i32 }

extern func @c_vfp_backfill(%a: f32, %b: f64, %c: f32, %d: f64, %e: f32) -> f64 cc "c" [nounwind]
extern func @c_make_hfa4f(%out: retbuf(@Hfa4f), %a: f32, %b: f32, %c: f32, %d: f32) -> void cc "c" [nounwind]
extern func @c_sum_va(%tag: i32, ...) -> i32 cc "c" [nounwind]

export func @pir_vfp_backfill(%a: f32, %b: f64, %c: f32, %d: f64, %e: f32) -> f64 cc "c" [nounwind] {
.entry:
    %ad = fpext.f64 %a
    %cd = fpext.f64 %c
    %ed = fpext.f64 %e
    %s1 = fadd.f64 %ad, %b
    %s2 = fadd.f64 %s1, %cd
    %s3 = fadd.f64 %s2, %d
    %s4 = fadd.f64 %s3, %ed
    ret %s4
}

export func @pir_make_hfa4f(%out: retbuf(@Hfa4f), %a: f32, %b: f32, %c: f32, %d: f32) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4f, %out, 0
    %p1 = field_ptr @Hfa4f, %out, 1
    %p2 = field_ptr @Hfa4f, %out, 2
    %p3 = field_ptr @Hfa4f, %out, 3
    store.f32 %a, %p0, align 4
    store.f32 %b, %p1, align 4
    store.f32 %c, %p2, align 4
    store.f32 %d, %p3, align 4
    ret
}

export func @pir_sum_hfa4f(%tag: f32, %x: f64, %h: byval(@Hfa4f)) -> f32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa4f, %h, 0
    %p1 = field_ptr @Hfa4f, %h, 1
    %p2 = field_ptr @Hfa4f, %h, 2
    %p3 = field_ptr @Hfa4f, %h, 3
    %a = load.f32 %p0, align 4
    %b = load.f32 %p1, align 4
    %c = load.f32 %p2, align 4
    %d = load.f32 %p3, align 4
    %xf = fptrunc.f32 %x
    %s0 = fadd.f32 %tag, %xf
    %s1 = fadd.f32 %a, %b
    %s2 = fadd.f32 %c, %d
    %s3 = fadd.f32 %s1, %s2
    %r  = fadd.f32 %s0, %s3
    ret %r
}

export func @pir_make_hfa3d(%out: retbuf(@Hfa3d), %a: f64, %b: f64, %c: f64) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Hfa3d, %out, 0
    %p1 = field_ptr @Hfa3d, %out, 1
    %p2 = field_ptr @Hfa3d, %out, 2
    store.f64 %a, %p0, align 8
    store.f64 %b, %p1, align 8
    store.f64 %c, %p2, align 8
    ret
}

export func @pir_make_small4(%out: retbuf(@Small4), %a: i8, %b: i16) -> void cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Small4, %out, 0
    %p1 = field_ptr @Small4, %out, 1
    store.i8 %a, %p0, align 1
    store.i16 %b, %p1, align 2
    ret
}

export func @pir_eval_mid12(%k0: i32, %k1: i32, %m: byval(@Mid12)) -> i32 cc "c" [nounwind] {
.entry:
    %p0 = field_ptr @Mid12, %m, 0
    %p1 = field_ptr @Mid12, %m, 1
    %p2 = field_ptr @Mid12, %m, 2
    %a = load.i32 %p0, align 4
    %b = load.i32 %p1, align 4
    %c = load.i32 %p2, align 4
    %s0 = add.i32 %k0, %k1
    %s1 = add.i32 %a, %b
    %s2 = add.i32 %s0, %s1
    %r  = add.i32 %s2, %c
    ret %r
}

export func @pir_i64_align(%t: i32, %x: i64, %y: i64) -> i64 cc "c" [nounwind] {
.entry:
    %t64 = zext.i64 %t
    %s1 = add.i64 %x, %y
    %r  = add.i64 %s1, %t64
    ret %r
}

export func @pir_run_checks() -> i32 cc "c" [nounwind] {
.entry:
    // PIR -> C: VFP backfilling
    %r1 = call @c_vfp_backfill(2.5, 3.25, 4.5, 5.75, 6.0) // 22.0
    %ok1 = fcmp_oeq.f64 %r1, 22.0
    br %ok1, .t2, .fail(10)

.t2:
    // PIR -> C: HFA4f return from clang-compiled C
    %h = alloca @Hfa4f
    call @c_make_hfa4f(%h, 1.5, 2.5, 3.5, 4.5)
    %p0 = field_ptr @Hfa4f, %h, 0
    %p1 = field_ptr @Hfa4f, %h, 1
    %p2 = field_ptr @Hfa4f, %h, 2
    %p3 = field_ptr @Hfa4f, %h, 3
    %v0 = load.f32 %p0, align 4
    %v1 = load.f32 %p1, align 4
    %v2 = load.f32 %p2, align 4
    %v3 = load.f32 %p3, align 4
    %s0 = fadd.f32 %v0, %v1
    %s1 = fadd.f32 %v2, %v3
    %st = fadd.f32 %s0, %s1
    %ok2 = fcmp_oeq.f32 %st, 12.0
    br %ok2, .t3, .fail(11)

.t3:
    // PIR -> C: variadic call (i32, f64, i64, i32) into clang-compiled C!
    %a = const.i32 10
    %d = const.f64 25.0
    %b = const.i64 30
    %c = const.i32 40
    %vr = call @c_sum_va(5, %a, %d, %b, %c) // 110
    %ok3 = cmp_eq.i32 %vr, 110
    br %ok3, .pass, .fail(12)

.pass:
    ret 0

.fail(%code: i32):
    ret %code
}
