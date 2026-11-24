// MODE: LIR
// EXPECT_RET: 92
target "x86_64-unknown-linux-gnu"

func @partition(%arr: ptr, %lo: i64, %hi: i64) -> i64 [nounwind] {
.entry:
    %hi_off = mul.i64 %hi, 4
    %piv_p = ptr_add.i64 %arr, %hi_off [inbounds]
    %pivot = load.i32 %piv_p, align 4
    jmp .loop(%lo, %lo)

.loop(%i: i64, %j: i64):
    %done = cmp_ge_s.i64 %j, %hi
    br %done, .finish, .body

.body:
    %j_off = mul.i64 %j, 4
    %jp = ptr_add.i64 %arr, %j_off [inbounds]
    %aj = load.i32 %jp, align 4
    %le = cmp_le_s.i32 %aj, %pivot
    %j1 = add.i64 %j, 1
    br %le, .do_swap, .loop(%i, %j1)

.do_swap:
    %i_off_s = mul.i64 %i, 4
    %ip_s = ptr_add.i64 %arr, %i_off_s [inbounds]
    %ai_s = load.i32 %ip_s, align 4
    store.i32 %aj, %ip_s, align 4
    store.i32 %ai_s, %jp, align 4
    %i1 = add.i64 %i, 1
    jmp .loop(%i1, %j1)

.finish:
    %i_off_f = mul.i64 %i, 4
    %ip_f = ptr_add.i64 %arr, %i_off_f [inbounds]
    %ai_f = load.i32 %ip_f, align 4
    store.i32 %pivot, %ip_f, align 4
    store.i32 %ai_f, %piv_p, align 4
    ret %i
}

func @qsort(%arr: ptr, %lo: i64, %hi: i64) -> void [nounwind] {
.entry:
    %ok = cmp_lt_s.i64 %lo, %hi
    br %ok, .split, .done

.split:
    %p = call @partition(%arr, %lo, %hi)
    %pm1 = sub.i64 %p, 1
    %pp1 = add.i64 %p, 1
    call @qsort(%arr, %lo, %pm1)
    call @qsort(%arr, %pp1, %hi)
    ret

.done:
    ret
}

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Unsorted: [34, 7, 23, 32, 5, 62, 1, 19] -> sorted: [1, 5, 7, 19, 23, 32, 34, 62]
    %arr = alloca.ptr 32, align 4
    %p0 = ptr_add.i64 %arr, 0 [inbounds]
    %p1 = ptr_add.i64 %arr, 4 [inbounds]
    %p2 = ptr_add.i64 %arr, 8 [inbounds]
    %p3 = ptr_add.i64 %arr, 12 [inbounds]
    %p4 = ptr_add.i64 %arr, 16 [inbounds]
    %p5 = ptr_add.i64 %arr, 20 [inbounds]
    %p6 = ptr_add.i64 %arr, 24 [inbounds]
    %p7 = ptr_add.i64 %arr, 28 [inbounds]
    store.i32 34, %p0, align 4
    store.i32 7, %p1, align 4
    store.i32 23, %p2, align 4
    store.i32 32, %p3, align 4
    store.i32 5, %p4, align 4
    store.i32 62, %p5, align 4
    store.i32 1, %p6, align 4
    store.i32 19, %p7, align 4

    call @qsort(%arr, 0, 7)
    jmp .verify(0)

.verify(%vi: i64):
    %done = cmp_ge_s.i64 %vi, 7
    br %done, .checksum, .vstep

.vstep:
    %o1 = mul.i64 %vi, 4
    %vi1 = add.i64 %vi, 1
    %o2 = mul.i64 %vi1, 4
    %q1 = ptr_add.i64 %arr, %o1 [inbounds]
    %q2 = ptr_add.i64 %arr, %o2 [inbounds]
    %v1 = load.i32 %q1, align 4
    %v2 = load.i32 %q2, align 4
    %sorted = cmp_le_s.i32 %v1, %v2
    br %sorted, .verify(%vi1), .fail

.checksum:
    // arr[0]=1, arr[3]=19, arr[6]=34, arr[7]=62 -> 1 + 19 + 34 + 62 - 24 = 92
    %a0 = load.i32 %p0, align 4
    %a3 = load.i32 %p3, align 4
    %a6 = load.i32 %p6, align 4
    %a7 = load.i32 %p7, align 4
    %s1 = add.i32 %a0, %a3
    %s2 = add.i32 %a6, %a7
    %s3 = add.i32 %s1, %s2
    %res = sub.i32 %s3, 24
    ret %res

.fail:
    trap
}
