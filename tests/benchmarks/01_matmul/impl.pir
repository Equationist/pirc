target "aarch64-apple-darwin"

extern func @malloc(%size: i64) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]

func @matmul_kernel(%C: ptr noalias, %A: ptr noalias readonly, %B: ptr noalias readonly, %n: i64) -> void [nounwind] {
.entry:
    jmp .loop_i(0)

.loop_i(%i: i64):
    %done_i = cmp_ge_s.i64 %i, %n
    br %done_i, .exit, .loop_j(0)

.loop_j(%j: i64):
    %done_j = cmp_ge_s.i64 %j, %n
    %i_next = add.i64 %i, 1 [unsafe_s]
    br %done_j, .loop_i(%i_next), .loop_k(0, 0.0)

.loop_k(%k: i64, %acc: f32):
    %done_k = cmp_ge_s.i64 %k, %n
    br %done_k, .store_c, .body_k

.body_k:
    %in = mul.i64 %i, %n [unsafe_s]
    %a_idx = add.i64 %in, %k [unsafe_s]
    %a_off = shl.i64 %a_idx, 2 [mask]
    %kn = mul.i64 %k, %n [unsafe_s]
    %b_idx = add.i64 %kn, %j [unsafe_s]
    %b_off = shl.i64 %b_idx, 2 [mask]
    %pa = ptr_add.i64 %A, %a_off [inbounds]
    %pb = ptr_add.i64 %B, %b_off [inbounds]
    %va = load.f32 %pa, align 4
    %vb = load.f32 %pb, align 4
    %prod = fmul.f32 %va, %vb
    %acc1 = fadd.f32 %acc, %prod
    %k1 = add.i64 %k, 1 [unsafe_s]
    jmp .loop_k(%k1, %acc1)

.store_c:
    %cin = mul.i64 %i, %n [unsafe_s]
    %c_idx = add.i64 %cin, %j [unsafe_s]
    %c_off = shl.i64 %c_idx, 2 [mask]
    %pc = ptr_add.i64 %C, %c_off [inbounds]
    store.f32 %acc, %pc, align 4
    %j1 = add.i64 %j, 1 [unsafe_s]
    jmp .loop_j(%j1)

.exit:
    ret
}

export func @bench_run() -> i64 cc "c" [nounwind] {
.entry:
    %N = const.i64 128
    %M = const.i64 16384
    %bytes = const.i64 65536
    %A = call @malloc(%bytes)
    %B = call @malloc(%bytes)
    %C = call @malloc(%bytes)
    jmp .init(0)

.init(%idx: i64):
    %id = cmp_ge_s.i64 %idx, %M
    br %id, .run_k, .init_body

.init_body:
    %t1 = mul.i64 %idx, 7
    %t2 = add.i64 %t1, 3
    %t3 = and.i64 %t2, 15
    %fa0 = sitofp.f32 %t3
    %fa = fmul.f32 %fa0, 0.25

    %u1 = mul.i64 %idx, 11
    %u2 = add.i64 %u1, 5
    %u3 = and.i64 %u2, 15
    %fb0 = sitofp.f32 %u3
    %fb = fmul.f32 %fb0, 0.25

    %off = shl.i64 %idx, 2 [mask]
    %pa = ptr_add.i64 %A, %off [inbounds]
    %pb = ptr_add.i64 %B, %off [inbounds]
    store.f32 %fa, %pa, align 4
    store.f32 %fb, %pb, align 4
    %idx1 = add.i64 %idx, 1
    jmp .init(%idx1)

.run_k:
    call @matmul_kernel(%C, %A, %B, %N)
    jmp .chk(0, 0)

.chk(%ci: i64, %sum: i64):
    %cd = cmp_ge_s.i64 %ci, %M
    br %cd, .done(%sum), .chk_body

.chk_body:
    %coff = shl.i64 %ci, 2 [mask]
    %pc = ptr_add.i64 %C, %coff [inbounds]
    %vc = load.f32 %pc, align 4
    %vi = fptosi.i64 %vc [sat]
    %sum1 = add.i64 %sum, %vi
    %ci1 = add.i64 %ci, 1
    jmp .chk(%ci1, %sum1)

.done(%final: i64):
    call @free(%A)
    call @free(%B)
    call @free(%C)
    ret %final
}
