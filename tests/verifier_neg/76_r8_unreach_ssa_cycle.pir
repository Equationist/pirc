// Reject cross-block SSA definition cycle in unreachable blocks
target "aarch64-apple-darwin"

export func @f() -> i64 {
.entry:
    ret 0
.dead1:
    %a = add.i64 %b, 1 [wrap]
    jmp .dead2
.dead2:
    %b = add.i64 %a, 1 [wrap]
    jmp .dead1
}
