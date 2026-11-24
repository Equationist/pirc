// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_LIR_FORBIDS: 12345
// EXPECT_LIR_CONTAINS: @side_effect
// EXPECT_LIR_CONTAINS: trap
// EXPECT_NO_CRITICAL_EDGES: true
target "aarch64-apple-darwin"

extern func @side_effect(%x: i32) -> i32 cc "c" [nounwind]

// 1. DCE removes dead pure ops (12345) while preserving side-effecting calls and reachable trap.
export func @dce_test(%x: i32, %fail: i8) -> i32 cc "c" [nounwind] {
.entry:
    %dead = mul.i32 %x, 12345
    %dead2 = add.i32 %dead, 7
    call @side_effect(%x)
    br %fail, .do_trap, .ok

.do_trap:
    trap

.ok:
    ret %x
}

// 2. EdgeSplit splits critical edge (.entry -> .join) because .entry has 2 succs and .join has 2 preds.
export func @critical_edge_test(%cond: i8, %a: i32, %b: i32) -> i32 cc "c" [nounwind] {
.entry:
    br %cond, .join(%a), .alt

.alt:
    %b2 = add.i32 %b, 1
    jmp .join(%b2)

.join(%v: i32):
    ret %v
}
