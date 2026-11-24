// MODE: LIR
// OPT_LEVEL: -O2
// EXPECT_RET: 42
// EXPECT_LIR_FORBIDS: .dead_block
// EXPECT_LIR_FORBIDS: .dead_const
// EXPECT_LIR_FORBIDS: .mid
// EXPECT_LIR_FORBIDS: br 
target "aarch64-apple-darwin"

// simplify_cfg must prune unreachable blocks, fold constant br, and merge single-pred chains.
export func @main() -> i32 cc "c" [nounwind] {
.entry:
    %c = const.i8 1
    br %c, .mid(20), .dead_const(99)

.mid(%a: i32):
    %b = add.i32 %a, 22
    jmp .tail(%b)

.tail(%out: i32):
    ret %out

.dead_const(%bad: i32):
    ret %bad

.dead_block:
    trap
}
