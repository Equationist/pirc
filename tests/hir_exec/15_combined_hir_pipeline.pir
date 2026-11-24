// MODE: HIR
// TEST_SIMD_CAPS: 256, 128, 0
// TEST_CAPS_MINIMAL: true
// EXPECT_RET: 95
target "x86_64-unknown-linux-gnu"

type @Record = { i8, i128, v4i32 }
type @Table = [2 x @Record]

export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %tbl = alloca @Table, align 16
    %r0 = elem_ptr @Table, %tbl, 0
    %r1 = elem_ptr @Table, %tbl, 1

    // Zero-init record 0 (including padding bytes) before populating fields
    memset %r0, 0, 48, align 16
    // Populate record 0
    %t0 = field_ptr @Record, %r0, 0
    %w0 = field_ptr @Record, %r0, 1
    %v0 = field_ptr @Record, %r0, 2
    store.i8 5, %t0, align 1
    %big = shl.i128 1, 64 [mask]
    %w_init = add.i128 %big, 30
    store.i128 %w_init, %w0, align 16
    %vec_init = splat.v4i32 10
    store.v4i32 %vec_init, %v0, align 16

    // Copy record 0 -> record 1 (struct size = 48 bytes: i8 + pad15 + i128 + v4i32)
    memcpy %r1, %r0, 48, align 16

    // Mutate record 1: add 20 to i128, add 5 per lane to v4i32
    %t1 = field_ptr @Record, %r1, 0
    %w1 = field_ptr @Record, %r1, 1
    %v1 = field_ptr @Record, %r1, 2
    %tag_val = load.i8 %t1, align 1
    %w_ld = load.i128 %w1, align 16
    %w_new = add.i128 %w_ld, 20
    %v_ld = load.v4i32 %v1, align 16
    %v_inc = splat.v4i32 5
    %v_new = add.v4i32 %v_ld, %v_inc

    // Extract components: tag=5, low(w_new)=50, lane0+lane3 of v_new = 15+15 = 30
    %hi_chk128 = shr.i128 %w_new, 64 [mask]
    %hi_chk = trunc.i64 %hi_chk128
    %h_ok = cmp_eq.i64 %hi_chk, 1
    br %h_ok, .combine, .fail

.combine:
    %tag32 = zext.i32 %tag_val
    %w_lo = trunc.i32 %w_new
    %vl0 = extract.v4i32 %v_new, 0
    %vl3 = extract.v4i32 %v_new, 3
    %vsum = add.i32 %vl0, %vl3
    // 5 + 50 + 30 = 85; switch on tag 5 adds 10 -> 95
    %base_sum = add.i32 %tag32, %w_lo
    %tot = add.i32 %base_sum, %vsum
    switch.i32 %tag32, default .fail [
        5 -> .finish(%tot)
    ]

.finish(%ans: i32):
    %res = add.i32 %ans, 10
    ret %res

.fail:
    trap
}
