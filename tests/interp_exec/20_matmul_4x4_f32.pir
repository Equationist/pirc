// MODE: LIR
// EXPECT_RET: 100
target "x86_64-unknown-linux-gnu"

// 4x4 f32 matrix multiplication: A[i,j] = (i+1), B[i,j] = (j+1)
// C[i,j] = sum_{k=0..3} (i+1)*(j+1) = 4*(i+1)*(j+1)
// Trace(C) = 4*(1*1 + 2*2 + 3*3 + 4*4) = 4*(1+4+9+16) = 120; return 120 - 20 = 100
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %A = alloca.ptr 64, align 16
    %B = alloca.ptr 64, align 16
    %C = alloca.ptr 64, align 16
    jmp .init_i(0)

.init_i(%ii: i64):
    %ii_done = cmp_ge_u.i64 %ii, 4
    br %ii_done, .mul_i(0), .init_j(0)

.init_j(%ij: i64):
    %ij_done = cmp_ge_u.i64 %ij, 4
    %ii_next = add.i64 %ii, 1
    br %ij_done, .init_i(%ii_next), .init_body

.init_body:
    %row_i = mul.i64 %ii, 4
    %idx_i = add.i64 %row_i, %ij
    %off_i = mul.i64 %idx_i, 4
    %pa_i = ptr_add.i64 %A, %off_i [inbounds]
    %pb_i = ptr_add.i64 %B, %off_i [inbounds]
    %iv = add.i64 %ii, 1
    %jv = add.i64 %ij, 1
    %fa = sitofp.f32 %iv
    %fb = sitofp.f32 %jv
    store.f32 %fa, %pa_i, align 4
    store.f32 %fb, %pb_i, align 4
    %ij_next = add.i64 %ij, 1
    jmp .init_j(%ij_next)

.mul_i(%mi: i64):
    %mi_done = cmp_ge_u.i64 %mi, 4
    br %mi_done, .trace(0, 0.0), .mul_j(0)

.mul_j(%mj: i64):
    %mj_done = cmp_ge_u.i64 %mj, 4
    %mi_next = add.i64 %mi, 1
    br %mj_done, .mul_i(%mi_next), .mul_k(0, 0.0)

.mul_k(%mk: i64, %acc_k: f32):
    %mk_done = cmp_ge_u.i64 %mk, 4
    br %mk_done, .store_c, .k_body

.k_body:
    %mi4 = mul.i64 %mi, 4
    %ai = add.i64 %mi4, %mk
    %ao = mul.i64 %ai, 4
    %mk4 = mul.i64 %mk, 4
    %bi = add.i64 %mk4, %mj
    %bo = mul.i64 %bi, 4
    %pa_m = ptr_add.i64 %A, %ao [inbounds]
    %pb_m = ptr_add.i64 %B, %bo [inbounds]
    %va = load.f32 %pa_m, align 4
    %vb = load.f32 %pb_m, align 4
    %prod = fmul.f32 %va, %vb [fast]
    %acc_k1 = fadd.f32 %acc_k, %prod [fast]
    %mk_next = add.i64 %mk, 1
    jmp .mul_k(%mk_next, %acc_k1)

.store_c:
    %ci4 = mul.i64 %mi, 4
    %ci = add.i64 %ci4, %mj
    %co = mul.i64 %ci, 4
    %pc_s = ptr_add.i64 %C, %co [inbounds]
    store.f32 %acc_k, %pc_s, align 4
    %mj_next = add.i64 %mj, 1
    jmp .mul_j(%mj_next)

.trace(%ti: i64, %tacc: f32):
    %ti_done = cmp_ge_u.i64 %ti, 4
    br %ti_done, .finish, .trace_step

.trace_step:
    %tidx = mul.i64 %ti, 5
    %toff = mul.i64 %tidx, 4
    %pc_t = ptr_add.i64 %C, %toff [inbounds]
    %tv = load.f32 %pc_t, align 4
    %tacc1 = fadd.f32 %tacc, %tv
    %ti_next = add.i64 %ti, 1
    jmp .trace(%ti_next, %tacc1)

.finish:
    %t_int = fptosi.i32 %tacc [sat]
    %res = sub.i32 %t_int, 20
    ret %res
}
