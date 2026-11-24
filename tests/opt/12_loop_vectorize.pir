// OPT_LEVEL: -O2
// EXPECT_RET: 0
// EXPECT_LIR_CONTAINS: load.v4f32
// EXPECT_LIR_CONTAINS: store.v4f32
// Regression test: general LIR inner-loop auto-vectorizer on a unit-stride
// f32 clamp loop (including non-multiple-of-8 tail, n=0, n=-3, and aliasing safety).

target "aarch64-apple-darwin"

extern func @malloc(%sz: i64) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]

func @vec_clamp(%dst: ptr noalias, %src: ptr noalias, %n: i64, %gain: f32, %lo: f32, %hi: f32) -> void cc "c" {
.entry:
    jmp .cond(0)

.cond(%i: i64):
    %ok = cmp_lt_s.i64 %i, %n
    br %ok, .body, .exit

.body:
    %off = shl.i64 %i, 2 [mask]
    %sp = ptr_add.i64 %src, %off
    %v0 = load.f32 %sp, align 4
    %v1 = fmul.f32 %v0, %gain
    %gt = fcmp_ogt.f32 %v1, %hi
    %v2 = select.f32 %gt, %hi, %v1
    %lt = fcmp_olt.f32 %v2, %lo
    %v3 = select.f32 %lt, %lo, %v2
    %dp = ptr_add.i64 %dst, %off
    store.f32 %v3, %dp, align 4
    %i1 = add.i64 %i, 1 [wrap]
    jmp .cond(%i1)

.exit:
    ret
}

export func @main() -> i32 cc "c" {
.entry:
    %src = call @malloc(128)
    %dst = call @malloc(128)
    jmp .init(0)

.init(%k: i64):
    %kdone = cmp_ge_s.i64 %k, 19
    br %kdone, .run, .init_b

.init_b:
    %fk = sitofp.f32 %k
    %fv = fsub.f32 %fk, 9.0
    %off = shl.i64 %k, 2 [mask]
    %p = ptr_add.i64 %src, %off
    store.f32 %fv, %p, align 4
    %k1 = add.i64 %k, 1 [wrap]
    jmp .init(%k1)

.run:
    call @vec_clamp(%dst, %src, 0, 2.0, -5.0, 5.0)
    call @vec_clamp(%dst, %src, -3, 2.0, -5.0, 5.0)
    call @vec_clamp(%dst, %src, 19, 2.0, -5.0, 5.0)
    // Check dst[0] == -5.0, dst[9] == 0.0, dst[10] == 2.0, dst[18] == 5.0
    %p0 = ptr_add.i64 %dst, 0
    %d0 = load.f32 %p0, align 4
    %p9 = ptr_add.i64 %dst, 36
    %d9 = load.f32 %p9, align 4
    %p10 = ptr_add.i64 %dst, 40
    %d10 = load.f32 %p10, align 4
    %p18 = ptr_add.i64 %dst, 72
    %d18 = load.f32 %p18, align 4
    %c0 = fcmp_oeq.f32 %d0, -5.0
    %c9 = fcmp_oeq.f32 %d9, 0.0
    %c10 = fcmp_oeq.f32 %d10, 2.0
    %c18 = fcmp_oeq.f32 %d18, 5.0
    %a1 = and.i8 %c0, %c9
    %a2 = and.i8 %c10, %c18
    %all = and.i8 %a1, %a2
    call @free(%src)
    call @free(%dst)
    %res = select.i32 %all, 0, 1
    ret %res
}
