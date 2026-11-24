target "x86_64-unknown-linux-gnu"

export func @sum_to(%n: i32) -> i32 {
.entry:
    jmp .loop(0, 0)

.loop(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit(%acc), .body(%i, %acc)

.body(%i: i32, %acc: i32):
    %next_acc = add.i32 %acc, %i
    %next_i   = add.i32 %i, 1 [unsafe_s]
    jmp .loop(%next_i, %next_acc)

.exit(%total: i32):
    ret %total
}
