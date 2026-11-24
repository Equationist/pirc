target "aarch64-apple-darwin"

extern func @malloc(%size: i64) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]

func @simd_clamp_kernel(%dst: ptr noalias, %src: ptr noalias readonly, %n: i64, %gain: f32, %lo: f32, %hi: f32) -> void [nounwind] {
.entry:
    %vg = splat.v4f32 %gain
    %vlo = splat.v4f32 %lo
    %vhi = splat.v4f32 %hi
    jmp .loop(0)

.loop(%i: i64):
    %done = cmp_ge_s.i64 %i, %n
    br %done, .exit, .body

.body:
    %off = shl.i64 %i, 2 [mask]
    %sp = ptr_add.i64 %src, %off [inbounds]
    %dp = ptr_add.i64 %dst, %off [inbounds]
    %v = load.v4f32 %sp, align 16
    %sc = fmul.v4f32 %v, %vg [fast]
    %gt = fcmp_ogt.v4f32 %sc, %vhi
    %c1 = select.v4f32 %gt, %vhi, %sc
    %lt = fcmp_olt.v4f32 %c1, %vlo
    %c2 = select.v4f32 %lt, %vlo, %c1
    store.v4f32 %c2, %dp, align 16
    %i1 = add.i64 %i, 4 [unsafe_s]
    jmp .loop(%i1)

.exit:
    ret
}

export func @bench_run() -> i64 cc "c" [nounwind] {
.entry:
    %N = const.i64 262144
    %bytes = const.i64 1048576
    %src = call @malloc(%bytes)
    %dst = call @malloc(%bytes)
    jmp .init(0)

.init(%i: i64):
    %id = cmp_ge_s.i64 %i, %N
    br %id, .reps(0), .init_body

.init_body:
    %m = and.i64 %i, 31
    %s = sub.i64 %m, 16
    %f0 = sitofp.f32 %s
    %f1 = fmul.f32 %f0, 0.25
    %off = shl.i64 %i, 2 [mask]
    %sp = ptr_add.i64 %src, %off [inbounds]
    store.f32 %f1, %sp, align 4
    %i1 = add.i64 %i, 1
    jmp .init(%i1)

.reps(%r: i32):
    %rd = cmp_ge_s.i32 %r, 64
    br %rd, .chk(0, 0), .rep_body

.rep_body:
    call @simd_clamp_kernel(%dst, %src, %N, 1.5, -2.0, 2.0)
    %r1 = add.i32 %r, 1
    jmp .reps(%r1)

.chk(%ci: i64, %acc: i64):
    %cd = cmp_ge_s.i64 %ci, %N
    br %cd, .done(%acc), .chk_body

.chk_body:
    %coff = shl.i64 %ci, 2 [mask]
    %dp = ptr_add.i64 %dst, %coff [inbounds]
    %dv = load.f32 %dp, align 4
    %d16 = fmul.f32 %dv, 16.0
    %q32 = fptosi.i32 %d16 [sat]
    %q64 = zext.i64 %q32
    %a1 = mul.i64 %acc, 131 [wrap]
    %acc1 = add.i64 %a1, %q64 [wrap]
    %ci1 = add.i64 %ci, 64
    jmp .chk(%ci1, %acc1)

.done(%res: i64):
    call @free(%src)
    call @free(%dst)
    ret %res
}
