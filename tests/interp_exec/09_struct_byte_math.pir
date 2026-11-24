// MODE: LIR
// EXPECT_RET: 70
target "x86_64-unknown-linux-gnu"

// Simulates lowered array of 4 structs of layout { i8 tag (off 0), pad 3, i32 w (off 4), i32 h (off 8) } -> stride 12
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %arr = alloca.ptr 48, align 4
    jmp .init_loop(0)

.init_loop(%i: i64):
    %init_done = cmp_ge_u.i64 %i, 4
    br %init_done, .sum_start, .init_body

.init_body:
    %off_i = mul.i64 %i, 12
    %elem_i = ptr_add.i64 %arr, %off_i [inbounds]
    %i32v = trunc.i32 %i
    %tag_i = trunc.i8 %i
    %w_i = add.i32 %i32v, 2
    %h_i = add.i32 %i32v, 3
    store.i8 %tag_i, %elem_i, align 1
    %wp_i = ptr_add.i64 %elem_i, 4 [inbounds]
    %hp_i = ptr_add.i64 %elem_i, 8 [inbounds]
    store.i32 %w_i, %wp_i, align 4
    store.i32 %h_i, %hp_i, align 4
    %i_next = add.i64 %i, 1
    jmp .init_loop(%i_next)

.sum_start:
    // Areas: (2*3) + (3*4) + (4*5) + (5*6) = 6 + 12 + 20 + 30 = 68; tags = 0+1+2+3 = 6 -> 74 - 4 = 70
    jmp .sum_loop(0, 0)

.sum_loop(%j: i64, %acc: i32):
    %sum_done = cmp_ge_u.i64 %j, 4
    br %sum_done, .finish(%acc), .sum_body

.sum_body:
    %off_j = mul.i64 %j, 12
    %elem_j = ptr_add.i64 %arr, %off_j [inbounds]
    %tag8 = load.i8 %elem_j, align 1
    %tag_j = zext.i32 %tag8
    %wp_j = ptr_add.i64 %elem_j, 4 [inbounds]
    %hp_j = ptr_add.i64 %elem_j, 8 [inbounds]
    %w_j = load.i32 %wp_j, align 4
    %h_j = load.i32 %hp_j, align 4
    %area = mul.i32 %w_j, %h_j
    %t1 = add.i32 %acc, %area
    %t2 = add.i32 %t1, %tag_j
    %j_next = add.i64 %j, 1
    jmp .sum_loop(%j_next, %t2)

.finish(%total: i32):
    %res = sub.i32 %total, 4
    ret %res
}
