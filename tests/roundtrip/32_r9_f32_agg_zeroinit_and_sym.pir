target "x86_64-unknown-linux-gnu"

type @F32Pair = { f32, f32 }
type @TwoInts = { i32, i32 }
type @ArrInts = [3 x i32]

@f32_agg = data @F32Pair = { 1.2345678806304932, -0.10000000149011612 }, align 4
@lead_z_struct = global @TwoInts = { zeroinit, 42 }, align 4
@lead_z_arr = global @ArrInts = { zeroinit, 10, 20 }, align 4
@base = global [4 x i64] = zeroinit, align 8
@rel_bare = data align 8 { @base + 16, @base - 8 }

export func @sym_no_space() -> ptr {
.entry:
    %p = const.ptr @base-16
    ret %p
}

export func @unreach_bp(%cond: i8) -> i32 {
.entry:
    ret 0
.dead_a(%x: i32):
    jmp .dead_use
.dead_b(%x: i32):
    ret %x
.dead_use:
    ret %x
}
