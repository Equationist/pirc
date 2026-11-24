// MODE: LIR
// EXPECT_RET: 53
target "x86_64-unknown-linux-gnu"

@greeting = data align 1 { "PIR!", 0 }
@g_counter = global i32 = 20, align 4
@g_bss = global i64 = zeroinit, align 8
@t_accum = tls i32 = 7, align 4
@g_nums = data align 4 { 10, 0, 0, 0, 26, 0, 0, 0 }
@g_second_ptr = data align 8 { ptr @g_nums + 4 }

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    // Check rodata byte 'P' (80)
    %gp = const.ptr @greeting
    %ch = load.i8 %gp, align 1
    %c_ok = cmp_eq.i8 %ch, 80
    // Check zeroinit bss
    %bp = const.ptr @g_bss
    %b0 = load.i64 %bp, align 8
    %b_ok = cmp_eq.i64 %b0, 0
    %ok = and.i8 %c_ok, %b_ok
    br %ok, .update, .fail

.update:
    // Mutate global: 20 -> 25
    %cp = const.ptr @g_counter
    %cv = load.i32 %cp, align 4
    %cv1 = add.i32 %cv, 5
    store.i32 %cv1, %cp, align 4
    // Mutate tls: 7 -> 9
    %tp = const.ptr @t_accum
    %tv = load.i32 %tp, align 4
    %tv1 = add.i32 %tv, 2
    store.i32 %tv1, %tp, align 4
    // Load via symbol+offset relocation -> points to 26
    %sp = const.ptr @g_second_ptr
    %target_p = load.ptr %sp, align 8
    %num2 = load.i32 %target_p, align 4
    // Reload global (25) + tls (9) + num2 (26) - 7 = 53
    %cv2 = load.i32 %cp, align 4
    %tv2 = load.i32 %tp, align 4
    %s1 = add.i32 %cv2, %tv2
    %s2 = add.i32 %s1, %num2
    %res = sub.i32 %s2, 7
    ret %res

.fail:
    trap
}
