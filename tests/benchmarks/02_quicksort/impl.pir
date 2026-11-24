target "aarch64-apple-darwin"

extern func @malloc(%size: i64) -> ptr cc "c" [nounwind]
extern func @free(%p: ptr) -> void cc "c" [nounwind]

func @partition(%arr: ptr noalias, %lo: i64, %hi: i64) -> i64 [nounwind] {
.entry:
    %hi_off = shl.i64 %hi, 2 [mask]
    %piv_p = ptr_add.i64 %arr, %hi_off [inbounds]
    %pivot = load.i32 %piv_p, align 4
    jmp .loop(%lo, %lo)

.loop(%i: i64, %j: i64):
    %done = cmp_ge_s.i64 %j, %hi
    br %done, .finish, .body

.body:
    %j_off = shl.i64 %j, 2 [mask]
    %jp = ptr_add.i64 %arr, %j_off [inbounds]
    %aj = load.i32 %jp, align 4
    %le = cmp_le_s.i32 %aj, %pivot
    %j1 = add.i64 %j, 1 [unsafe_s]
    br %le, .swap, .loop(%i, %j1)

.swap:
    %i_off = shl.i64 %i, 2 [mask]
    %ip = ptr_add.i64 %arr, %i_off [inbounds]
    %ai = load.i32 %ip, align 4
    store.i32 %aj, %ip, align 4
    store.i32 %ai, %jp, align 4
    %i1 = add.i64 %i, 1 [unsafe_s]
    jmp .loop(%i1, %j1)

.finish:
    %fi_off = shl.i64 %i, 2 [mask]
    %fip = ptr_add.i64 %arr, %fi_off [inbounds]
    %fai = load.i32 %fip, align 4
    store.i32 %pivot, %fip, align 4
    store.i32 %fai, %piv_p, align 4
    ret %i
}

func @qsort_rec(%arr: ptr noalias, %lo: i64, %hi: i64) -> void [nounwind] {
.entry:
    %ok = cmp_lt_s.i64 %lo, %hi
    br %ok, .split, .done

.split:
    %p = call @partition(%arr, %lo, %hi)
    %pm1 = sub.i64 %p, 1 [unsafe_s]
    %pp1 = add.i64 %p, 1 [unsafe_s]
    call @qsort_rec(%arr, %lo, %pm1)
    call @qsort_rec(%arr, %pp1, %hi)
    ret

.done:
    ret
}

export func @bench_run() -> i64 cc "c" [nounwind] {
.entry:
    %N = const.i64 65536
    %bytes = const.i64 262144
    %arr = call @malloc(%bytes)
    jmp .seed(0, 0x12345678)

.seed(%si: i64, %st: i32):
    %sd = cmp_ge_s.i64 %si, %N
    br %sd, .run_sort, .seed_body

.seed_body:
    %m = mul.i32 %st, 1664525 [wrap]
    %st1 = add.i32 %m, 1013904223 [wrap]
    %off = shl.i64 %si, 2 [mask]
    %p = ptr_add.i64 %arr, %off [inbounds]
    store.i32 %st1, %p, align 4
    %si1 = add.i64 %si, 1
    jmp .seed(%si1, %st1)

.run_sort:
    call @qsort_rec(%arr, 0, 65535)
    jmp .chk(0, 0)

.chk(%ci: i64, %acc: i64):
    %cd = cmp_ge_s.i64 %ci, %N
    br %cd, .done(%acc), .chk_body

.chk_body:
    %coff = shl.i64 %ci, 2 [mask]
    %cp = ptr_add.i64 %arr, %coff [inbounds]
    %v32 = load.i32 %cp, align 4
    %v64 = zext.i64 %v32
    %a31 = mul.i64 %acc, 31 [wrap]
    %acc1 = add.i64 %a31, %v64 [wrap]
    %ci1 = add.i64 %ci, 1
    jmp .chk(%ci1, %acc1)

.done(%res: i64):
    call @free(%arr)
    ret %res
}
