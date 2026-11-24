// MODE: HIR
// EXPECT_ERR: use of '%v' in '.join' is not dominated by definition in '.left'
target "x86_64-unknown-linux-gnu"

func @bad(%c: i8) -> i32 {
.entry:
    br %c, .left, .right
.left:
    %v = const.i32 7
    jmp .join
.right:
    jmp .join
.join:
    ret %v
}
