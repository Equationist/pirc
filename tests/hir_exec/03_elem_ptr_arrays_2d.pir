// MODE: HIR
// EXPECT_RET: 82
target "x86_64-unknown-linux-gnu"

// @Item has non-power-of-two stride: i8 (off 0) + pad 3 + i32 (off 4) + i32 (off 8) = 12 bytes
type @Item = { i8, i32, i32 }
type @Items = [4 x @Item]
type @Grid = [3 x [4 x i32]]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %items = alloca @Items
    jmp .fill_items(0)

.fill_items(%i: i64):
    %done_i = cmp_ge_u.i64 %i, 4
    br %done_i, .sum_items_init, .fill_items_body

.fill_items_body:
    %ep = elem_ptr @Items, %items, %i
    %p_tag = field_ptr @Item, %ep, 0
    %p_x   = field_ptr @Item, %ep, 1
    %p_y   = field_ptr @Item, %ep, 2
    %i32v  = trunc.i32 %i
    %tag   = trunc.i8 %i
    %xv    = add.i32 %i32v, 1
    %yv    = mul.i32 %xv, 2
    store.i8 %tag, %p_tag, align 1
    store.i32 %xv, %p_x, align 4
    store.i32 %yv, %p_y, align 4
    %i_next = add.i64 %i, 1
    jmp .fill_items(%i_next)

.sum_items_init:
    // Sum over i=0..3 of (tag + x*y) = (0+2) + (1+8) + (2+18) + (3+32) = 2+9+20+35 = 66
    jmp .sum_items(0, 0)

.sum_items(%j: i64, %acc_i: i32):
    %done_j = cmp_ge_u.i64 %j, 4
    br %done_j, .grid_start(%acc_i), .sum_items_body

.sum_items_body:
    %ep2 = elem_ptr @Items, %items, %j
    %pt2 = field_ptr @Item, %ep2, 0
    %px2 = field_ptr @Item, %ep2, 1
    %py2 = field_ptr @Item, %ep2, 2
    %t8  = load.i8 %pt2, align 1
    %t32 = zext.i32 %t8
    %vx  = load.i32 %px2, align 4
    %vy  = load.i32 %py2, align 4
    %pr  = mul.i32 %vx, %vy
    %s1  = add.i32 %acc_i, %t32
    %s2  = add.i32 %s1, %pr
    %j1  = add.i64 %j, 1
    jmp .sum_items(%j1, %s2)

.grid_start(%item_sum: i32):
    // 2D array [3 x [4 x i32]]: write grid[2][3] = 16
    %grid = alloca @Grid
    %r_idx = const.i64 2
    %c_idx = const.i64 3
    %row_p = elem_ptr @Grid, %grid, %r_idx
    %cell_p = elem_ptr [4 x i32], %row_p, %c_idx
    store.i32 16, %cell_p, align 4
    %cell_v = load.i32 %cell_p, align 4
    // 66 + 16 = 82
    %res = add.i32 %item_sum, %cell_v
    ret %res
}
