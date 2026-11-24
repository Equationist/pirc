target "x86_64-unknown-linux-gnu"

export func @nested_cfg(%n: i32, %c: i8) -> i32 {
.entry:
    %base = const.i32 10
    br %c, .left, .right

.left:
    %l1 = add.i32 %base, %n
    jmp .outer(0, %l1)

.right:
    %r1 = sub.i32 %base, %n
    jmp .outer(0, %r1)

.outer(%i: i32, %acc: i32):
    %done = cmp_ge_s.i32 %i, %n
    br %done, .exit(%acc), .inner(0, %acc)

.inner(%j: i32, %sum: i32):
    %jdone = cmp_ge_s.i32 %j, %i
    %ns = add.i32 %sum, %j
    %nj = add.i32 %j, 1
    br %jdone, .outer_step(%i, %sum), .inner(%nj, %ns)

.outer_step(%oi: i32, %oacc: i32):
    %ni = add.i32 %oi, 1
    jmp .outer(%ni, %oacc)

.exit(%final: i32):
    %out = add.i32 %final, %base
    ret %out

.dead:
    unreachable
}
